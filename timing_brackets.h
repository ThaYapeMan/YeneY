// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "timing_probe.h"
#include <limits>
namespace timing_probe {
struct Bracket {
    double second, lo, hi;
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
    void reset(double previous = 0) {
        *this = BracketClock{};
        warm = previous;
    }
    double timeAt(double second) const { return reference + offset + (second - n0) * (1 + drift * 1e-6); }
    double rateDrift() const { return (1 / (1 + drift * 1e-6) - 1) * 1e6; }
    double uncertainty() const { return locked ? width * 500 + 1 : NAN; }
    bool add(Bracket e) {
        if (!std::isfinite(e.second) || !std::isfinite(e.lo) || !std::isfinite(e.hi) || e.hi < e.lo)
            return false;
        if (locked) {
            const double predicted = timeAt(e.second);
            failures = predicted < e.lo - .002 || predicted > e.hi + .002 ? failures + 1 : 0;
            if (failures >= 2) {
                reset(driftReady ? drift : warm);
            } else if (failures)
                return false; // A single unconfirmed mismatch cannot move the contract.
        }
        edges.push_back(e);
        while (edges.size() > 1200 || (!edges.empty() && e.second - edges.front().second > 600))
            edges.pop_front();
        fit();
        return true;
    }
    void fit() {
        if (edges.empty())
            return;
        n0 = edges.back().second;
        reference = (edges.back().lo + edges.back().hi) / 2;
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
            trials.push_back({0, band(0)});
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
        drift = selected;
        offset = (chosen.lo + chosen.hi) / 2;
        width = chosen.hi - chosen.lo;
        violators = unsigned(edges.size()) - best;
        fitted = true;
        driftReady = span >= 120;
        locked = span >= 60 && edges.size() >= 40 && width <= .015 && violators <= edges.size() * .02;
    }
};
} // namespace timing_probe
