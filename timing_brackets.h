// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "timing_probe.h"
#include <limits>
namespace timing_probe {
struct Bracket {
    double second, lo, hi;
    double observationWidth = 0;
};
// Interval consensus, independent of the midpoint comparison estimator.
class BracketClock {
    struct Band {
        double lo = 0, hi = 0;
        unsigned count = 0;
    };
    Band band(double ppm, unsigned minimum = 0) const {
        const double k = 1 + ppm * 1e-6;
        double lower = -INFINITY, upper = INFINITY;
        for (auto e : edges) {
            lower = std::max(lower, e.lo - reference - k * (e.second - n0));
            upper = std::min(upper, e.hi - reference - k * (e.second - n0));
        }
        if (lower <= upper)
            return {lower, upper, unsigned(edges.size())};
        if (minimum == edges.size())
            return {};
        std::vector<std::pair<double, int>> points;
        points.reserve(edges.size() * 2);
        for (auto e : edges) {
            points.push_back({e.lo - reference - k * (e.second - n0), 1});
            points.push_back({e.hi - reference - k * (e.second - n0), -1});
        }
        std::sort(points.begin(), points.end(), [](auto a, auto b) {
            return a.first != b.first ? a.first < b.first : a.second > b.second;
        });
        Band result;
        unsigned count = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            count += points[i].second;
            if (i + 1 < points.size() && points[i + 1].first >= points[i].first &&
                (count > result.count ||
                 (count == result.count && points[i + 1].first - points[i].first > result.hi - result.lo)))
                result = {points[i].first, points[i + 1].first, count};
        }
        return result;
    }

public:
    std::deque<Bracket> edges;
    double n0 = 0, reference = 0, offset = 0, drift = 0, width = NAN, span = 0;
    double lowDrift = 0, highDrift = 0;
    unsigned violators = 0, failures = 0;
    bool locked = false, fitted = false, driftReady = false;
    double warm = 0;
    bool prior = false, priorDropped = false;
    double priorDrift = 0, priorSigma = 0;
    void usePrior(double ppm, double sigma) {
        prior = true; priorDrift = ppm; priorSigma = sigma; warm = ppm;
    }
    unsigned oneSided = 0, contradictions = 0;
    double lastTwoSided = NAN;
    void reset(double previous = 0) {
        *this = BracketClock{};
        warm = previous;
    }
    double timeAt(double second) const { return reference + offset + (second - n0) * (1 + drift * 1e-6); }
    double rateDrift() const { return (1 / (1 + drift * 1e-6) - 1) * 1e6; }
    double uncertainty(double now = NAN) const {
        const double age =
            std::isfinite(now) && std::isfinite(lastTwoSided) ? std::max(0., now - lastTwoSided) : 0;
        return locked ? width * 500 + 1 + age * (.1 + std::max(0., highDrift - lowDrift) * .0005) : NAN;
    }
    bool add(Bracket e) {
        if (!std::isfinite(e.second) || std::isnan(e.lo) || std::isnan(e.hi) ||
            (!std::isfinite(e.lo) && !std::isfinite(e.hi)) || e.hi < e.lo)
            return false;
        if (locked) {
            const double predicted = timeAt(e.second);
            const double ownWidth =
                std::isfinite(e.lo) && std::isfinite(e.hi) ? e.hi - e.lo : e.observationWidth;
            const double tolerance = std::max(.002, ownWidth * .5);
            const bool inconsistent =
                predicted + width / 2 < e.lo - tolerance || predicted - width / 2 > e.hi + tolerance;
            if (inconsistent)
                ++contradictions;
            failures = inconsistent ? failures + 1 : 0;
            if (failures >= 2) {
                const bool rejectedPrior = prior;
                reset(rejectedPrior ? 0 : driftReady ? drift : warm);
                priorDropped = rejectedPrior;
            } else if (failures)
                return false; // A single unconfirmed mismatch cannot move the contract.
        }
        if (!std::isfinite(e.lo) || !std::isfinite(e.hi))
            ++oneSided;
        else
            lastTwoSided = (e.lo + e.hi) / 2;
        // One-sided send/receive bounds carry the bounding SOAP sample's latency.
        // Use the same tolerance as contradiction detection for interval consensus.
        if (!std::isfinite(e.lo))
            e.hi += std::max(.002, e.observationWidth * .5);
        if (!std::isfinite(e.hi))
            e.lo -= std::max(.002, e.observationWidth * .5);
        edges.push_back(e);
        while (edges.size() > 1200 || (!edges.empty() && e.second - edges.front().second > 600))
            edges.pop_front();
        fit();
        return true;
    }
    void fit() {
        if (edges.empty())
            return;
        // Preserve the previous finite model if a long one-sided-only window
        // cannot reacquire both ends of an offset band.
        if (locked && std::none_of(edges.begin(), edges.end(),
                                   [](Bracket e) { return std::isfinite(e.lo) && std::isfinite(e.hi); }))
            return;
        n0 = edges.back().second;
        const auto &recent = edges.back();
        reference = std::isfinite(recent.lo) && std::isfinite(recent.hi) ? (recent.lo + recent.hi) / 2
                    : std::isfinite(recent.lo)                           ? recent.lo
                                                                         : recent.hi;
        span = edges.back().second - edges.front().second;
        struct Trial {
            double ppm;
            Band b;
        };
        std::vector<Trial> trials;
        unsigned best = 0;
        const double searchCenter = driftReady ? drift : warm;
        const double searchLo = searchCenter - 200, searchHi = searchCenter + 200;
        auto scan = [&](double center, double radius, double step) {
            for (double p = center - radius; p <= center + radius + step * .1; p += step) {
                if (p < searchLo || p > searchHi)
                    continue;
                auto b = band(p, best);
                trials.push_back({p, b});
                best = std::max(best, b.count);
            }
        };
        // Unity before 120 s; a qualified room drift seeds the subsequent search.
        if (span < 120) {
            trials.push_back({prior ? priorDrift : 0, band(prior ? priorDrift : 0)});
            best = trials.back().b.count;
        } else {
            const double center = searchCenter;
            scan(center, 0, 1);
            scan(center, 200, 10);
            auto bounds = [&] {
                double lo = INFINITY, hi = -INFINITY;
                for (auto t : trials)
                    if (t.b.count == best) {
                        lo = std::min(lo, t.ppm);
                        hi = std::max(hi, t.ppm);
                    }
                return std::pair<double, double>{lo, hi};
            };
            auto range = bounds();
            scan(range.first, 10, 1);
            scan(range.second, 10, 1);
            range = bounds();
            scan(range.first, 1, .1);
            scan(range.second, 1, .1);
            range = bounds();
            scan((range.first + range.second) / 2, .1, .1);
        }
        lowDrift = INFINITY;
        highDrift = -INFINITY;

        for (auto t : trials)
            if (t.b.count == best) {
                lowDrift = std::min(lowDrift, t.ppm);
                highDrift = std::max(highDrift, t.ppm);
            }
        double selected = (lowDrift + highDrift) / 2;
        auto chosen = band(selected);
        // Never choose a midpoint of disconnected rate solutions with lower consensus.
        if (chosen.count != best) {
            auto closest = trials.front();
            double distance = INFINITY;
            for (auto t : trials)
                if (t.b.count == best && std::abs(t.ppm - selected) < distance) {
                    closest = t;
                    distance = std::abs(t.ppm - selected);
                }
            selected = closest.ppm;
            chosen = closest.b;
        }
        if (!std::isfinite(chosen.lo) || !std::isfinite(chosen.hi)) {
            fitted = false;
            width = NAN;
            return;
        }
        drift = selected;
        offset = (chosen.lo + chosen.hi) / 2;
        width = chosen.hi - chosen.lo;
        violators = unsigned(edges.size()) - best;
        fitted = true;
        if (prior && ((edges.size() >= 10 && violators > edges.size() * .02) ||
                      (span >= 120 && (priorDrift < lowDrift - 3 * priorSigma ||
                                       priorDrift > highDrift + 3 * priorSigma)))) {
            prior = false; priorDropped = true; locked = false; driftReady = false; warm = 0;
            fit();
            return;
        }
        if (prior && span < 120) {
            lowDrift = priorDrift - 2 * priorSigma;
            highDrift = priorDrift + 2 * priorSigma;
        }
        driftReady = span >= 120;
        locked =
            locked || ((prior ? edges.size() >= 5 : span >= 60 && edges.size() >= 40) && width <= .015 && violators <= edges.size() * .02);
    }
};
} // namespace timing_probe
