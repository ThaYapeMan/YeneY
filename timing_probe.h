// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Project-owned, measurement-only RelTime edge estimator. Times are seconds.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>
namespace timing_probe {
inline double percentile(std::vector<double> v, double q) {
    if (v.empty())
        return NAN;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(std::ceil(q * (v.size() - 1))))];
}
struct Edge {
    double time, second, width;
};
struct EdgeDecision {
    Edge edge{};
    double residual = NAN;
    bool observed = false, inlier = false;
    const char *rule = "none";
};
class Estimator {
  public:
    std::deque<Edge> edges;
    std::deque<double> rtts;
    std::vector<Edge> inliers;
    double origin = 0, intercept = 0, slope = 1, window = .2;
    double span = 0, driftSigma = NAN, spread = 0, lastUsable = 0;
    bool fitted = false, sampled = false, primed = false, driftReported = false,
         precisionFit = false;
    double lastTime = 0, lastSecond = 0, lastRtt = 0, due = 0;
    double missedSecond = -1;
    unsigned phase = 0, rejected = 0, missed = 0, outliers = 0, stalled = 0;
    bool recovered = false;
    double dither = 0;
    unsigned burst = 0;
    uint64_t usableEdges = 0;
    EdgeDecision decision;
    const char *sampleOutcome = "accepted", *sampleReason = "sample";
    void reset() { *this = Estimator{}; }
    void seed(double second, double time) {
        if (sampled || primed)
            return;
        primed = true;
        origin = time;
        intercept = second + .5;
        slope = 1;
        due = time + .25;
    }
    double rttLimit() const {
        if (rtts.size() < 12)
            return .080;
        return std::clamp(2 * percentile({rtts.begin(), rtts.end()}, .95), .010, .080);
    }
    double windowFloor() const {
        const double rtt = rtts.empty() ? 0 : percentile({rtts.begin(), rtts.end()}, .95);
        return std::clamp(3 * spread + rtt / 2, .020, .250);
    }
    void fit() {
        if (edges.empty())
            return;
        const double newOrigin = edges.back().time;
        std::vector<Edge> used(edges.begin(), edges.end());
        // Broad acquisition brackets can seed phase, but must not dominate a
        // mature precision fit or hide delayed ticks behind their large width.
        precisionFit =
            std::count_if(used.begin(), used.end(), [](Edge e) { return e.width <= .100; }) >= 20;
        if (precisionFit)
            used.erase(
                std::remove_if(used.begin(), used.end(), [](Edge e) { return e.width > .100; }),
                used.end());
        double rate = 1, offset = 0, sigma = NAN;
        // Classify against an unconstrained fit, including while drift is not yet
        // reportable: a real rate offset must not masquerade as phase outliers.
        for (unsigned pass = 0; pass <= 6 && !used.empty(); ++pass) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0, weight = 0;
            for (auto e : used) {
                double x = e.time - newOrigin, w = 1 / std::pow(std::max(.020, e.width), 2);
                weight += w;
                sx += w * x;
                sy += w * e.second;
                sxx += w * x * x;
                sxy += w * x * e.second;
            }
            const double denom = sxx - sx * sx / weight;
            rate = used.size() >= 10 && denom > 0 ? (sxy - sx * sy / weight) / denom : 1;
            if (std::abs(rate - 1) > .002)
                rate = 1;
            offset = (sy - rate * sx) / weight;
            std::vector<double> residual, deviations;
            for (auto e : used)
                residual.push_back((e.second - (offset + rate * (e.time - newOrigin))) / rate);
            const double median = percentile(residual, .5);
            for (double r : residual)
                deviations.push_back(std::abs(r - median));
            const double mad = 1.4826 * percentile(deviations, .5);
            std::vector<Edge> keep;
            double sse = 0;
            for (size_t i = 0; i < used.size(); ++i) {
                const auto e = used[i];
                if (pass == 6 ||
                    std::abs(residual[i] - median) <= std::max({3 * mad, e.width / 2, .010})) {
                    keep.push_back(e);
                    sse += std::pow(e.second - (offset + rate * (e.time - newOrigin)), 2) /
                           std::pow(std::max(.020, e.width), 2);
                }
            }
            if (keep.size() == used.size()) {
                spread = std::count_if(used.begin(), used.end(),
                                       [](Edge e) { return e.width <= .100; }) >= 20
                             ? mad
                             : 0;
                sigma = used.size() > 2 && denom > 0
                            ? std::sqrt(std::max(1. / 12, sse / (used.size() - 2)) / denom)
                            : NAN;
                break;
            }
            used.swap(keep);
        }
        if (used.empty())
            return; // Retain the previous model if all candidates are unusable.
        inliers = used;
        outliers = unsigned(edges.size() - inliers.size());
        span = inliers.back().time - inliers.front().time;
        driftSigma = sigma * 1e6;
        driftReported = span >= 120 && std::isfinite(sigma) && driftSigma <= 15 &&
                        std::abs(rate - 1) > 2 * sigma;
        slope = driftReported ? rate : 1;
        origin = newOrigin;
        // Recenter the offset when the rate is held at unity.
        double weight = 0, sum = 0;
        for (auto e : inliers) {
            if (!driftReported && inliers.back().time - e.time > 60)
                continue;
            const double w = 1 / std::pow(std::max(.020, e.width), 2);
            weight += w;
            sum += w * (e.second - slope * (e.time - origin));
        }
        intercept = sum / weight;
        fitted = true;
        window = std::max(window, windowFloor());
    }
    bool stale(double now, double started, unsigned seconds) const {
        return now - (usableEdges ? lastUsable : started) > seconds;
    }
    double timeAt(double second) const { return origin + (second - intercept) / slope; }
    // Every adjacent transition is observable, even when too broad to fit.
    bool sample(double sent, double received, double second) {
        decision = {};
        recovered = false;
        sampleOutcome = "accepted";
        sampleReason = "sample";
        const double rtt = received - sent, t = (sent + received) / 2;
        if (!std::isfinite(sent) || !std::isfinite(received) || rtt < 0 || !std::isfinite(second)) {
            sampleOutcome = "error";
            sampleReason = "invalid-sample";
            return false;
        }
        const double limit = rttLimit();
        rtts.push_back(rtt);
        if (rtts.size() > 128)
            rtts.pop_front();
        if (rtt > limit) {
            ++rejected;
            sampleOutcome = "rejected-rtt";
            sampleReason = "adaptive-rtt-limit";
            due = t + .34;
            return false;
        }
        bool edge = false;
        if (sampled && second > lastSecond + 1) {
            missed += unsigned(second - lastSecond - 1);
            window = std::min(.25, std::max(windowFloor(), window * 2));
        }
        if (sampled && second == lastSecond + 1 && t > lastTime) {
            const double width = t - lastTime + (rtt + lastRtt) / 2;
            const double center = (lastTime + t) / 2 + (rtt - lastRtt) / 4;
            decision = {{center, second, width},
                        fitted ? (center - timeAt(second)) * 1000 : NAN,
                        true,
                        false,
                        "wide-bracket"};
            if (width <= .65) {
                edges.push_back(decision.edge);
                if (edges.size() > 300)
                    edges.pop_front();
                fit();
                decision.inlier = std::any_of(inliers.begin(), inliers.end(),
                                              [center](Edge e) { return e.time == center; });
                decision.rule = decision.inlier                ? "median-mad-width-floor"
                                : precisionFit && width > .100 ? "wide-bracket"
                                                               : "median-mad-outlier";
                if (decision.inlier) {
                    ++usableEdges;
                    lastUsable = center;
                    edge = true;
                    window = std::max(windowFloor(), window * .25);
                }
            }
            if (!decision.inlier) {
                ++rejected;
                ++stalled;
            } else stalled = 0;
            // A bad acquisition phase must not schedule one rejected edge per
            // second forever. Keep RTT history and the caller's request budget.
            if (stalled >= 3 && inliers.size() < 20) {
                edges.clear(); inliers.clear(); fitted = primed = false;
                span = spread = 0; slope = 1; window = .2; phase = 0;
                stalled = 0; recovered = true;
            }
        }
        // Counter anomalies are observations, not authority to reset a model.
        // Existing transport/PCM-anchor hooks own discontinuity resets.
        if (sampled && (second < lastSecond || second - lastSecond > (t - lastTime) * 1.01 + 2)) {
            ++rejected;
            sampleReason = "counter-discontinuity";
            window = .25;
        }
        sampled = true;
        lastTime = t;
        lastSecond = second;
        lastRtt = rtt;
        if (decision.observed && !decision.inlier && inliers.size() < 20) {
            // Search immediately after a miss; extrapolating the same bad fit
            // here can place every subsequent request on the same tick side.
            phase = 0;
            window = .25;
            due = t + .34;
        } else if (edges.size() < 5) {
            due = t + .34;
        } else if (decision.observed) {
            phase = 0;
            // Deterministic phase dither prevents the sampling schedule locking
            // to one side of a quantized tick and biasing the drift regression.
            dither = (std::fmod(++burst * .6180339887498949, 1.) - .5) * 2 * std::min(window, .040);
            due = dither + timeAt(second + 1) - window -
                  percentile({rtts.begin(), rtts.end()}, .5) / 2;
        } else if (fitted || primed) {
            const double prediction =
                dither + timeAt(second + 1) - percentile({rtts.begin(), rtts.end()}, .5) / 2;
            const double near = std::min(window, .020);
            if (phase == 0 && t < prediction) {
                phase = 1;
                due = prediction;
            } else if (phase <= 1 && t < prediction + near) {
                phase = 2;
                due = prediction + near;
            } else {
                if (t >= prediction + window && missedSecond != second + 1) {
                    ++missed;
                    missedSecond = second + 1;
                    window = std::min(.25, std::max(windowFloor(), window * 2));
                }
                phase = 0;
                due = t + .34;
            }
        } else
            due = t + .34;
        return edge;
    }
    std::vector<double> residuals() const {
        std::vector<double> v;
        if (fitted)
            for (auto e : inliers)
                v.push_back(1000 * std::abs(e.second - (intercept + slope * (e.time - origin))) /
                            slope);
        return v;
    }
};
class Limiter {
    std::deque<double> requests;

  public:
    void prune(double t) {
        while (!requests.empty() && t - requests.front() >= 10)
            requests.pop_front();
    }
    bool take(double t) {
        prune(t);
        size_t recent = 0;
        for (double s : requests)
            if (t - s < 1)
                ++recent;
        if (recent >= 8 || requests.size() >= 30)
            return false;
        requests.push_back(t);
        return true;
    }
    // The dedicated worker has only one outstanding request. Move its budget
    // reservation to the captured first-send time once the reply completes.
    void sent(double t) {
        if (!requests.empty() && t > requests.back())
            requests.back() = t;
    }
    double rate(double t) {
        prune(t);
        return requests.size() / 10.;
    }
};
inline bool eligible(bool enabled, bool coordinator, bool playing, bool ours, bool allowed) {
    return enabled && coordinator && playing && ours && allowed;
}
} // namespace timing_probe
