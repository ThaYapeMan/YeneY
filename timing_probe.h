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
class Estimator {
  public:
    std::deque<Edge> edges;
    std::deque<double> rtts;
    std::vector<Edge> inliers;
    double origin = 0, intercept = 0, slope = 1, window = .2;
    bool fitted = false, sampled = false, primed = false;
    double lastTime = 0, lastSecond = 0, lastRtt = 0, due = 0;
    unsigned phase = 0, rejected = 0;
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
        // Bootstrap conservatively; learn a room-specific threshold thereafter.
        if (rtts.size() < 12)
            return .080;
        std::vector<double> v(rtts.begin(), rtts.end());
        return std::clamp(2 * percentile(v, .95), .010, .080);
    }
    void fit() {
        if (edges.empty())
            return;
        origin = edges.back().time;
        std::vector<Edge> used(edges.begin(), edges.end());
        for (unsigned pass = 0; pass < 3 && !used.empty(); ++pass) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
            for (auto e : used) {
                double x = e.time - origin, w = 1 / std::pow(std::max(.020, e.width), 2);
                n += w;
                sx += w * x;
                sy += w * e.second;
                sxx += w * x * x;
                sxy += w * x * e.second;
            }
            double denom = n * sxx - sx * sx;
            slope = used.size() >= 10 && denom > 0 ? (n * sxy - sx * sy) / denom : 1;
            // Refuse physically implausible drift; never repair playback clocks.
            if (std::abs(slope - 1) > .002)
                slope = 1;
            intercept = (sy - slope * sx) / n;
            std::vector<double> residual;
            for (auto e : used)
                residual.push_back(std::abs(e.second - (intercept + slope * (e.time - origin))));
            double cutoff = std::max(.030, 4 * percentile(residual, .5));
            std::vector<Edge> keep;
            for (auto e : used)
                if (std::abs(e.second - (intercept + slope * (e.time - origin))) <= cutoff + e.width / 2)
                    keep.push_back(e);
            used.swap(keep);
        }
        inliers = used;
        fitted = true;
    }
    double timeAt(double second) const { return origin + (second - intercept) / slope; }
    // Returns true only for a usable bracket. The endpoints include RTT bounds.
    bool sample(double sent, double received, double second) {
        const double rtt = received - sent, t = (sent + received) / 2;
        if (rtt < 0 || !std::isfinite(second))
            return false;
        const double limit = rttLimit();
        rtts.push_back(rtt);
        if (rtts.size() > 128)
            rtts.pop_front();
        if (rtt > limit) {
            ++rejected;
            due = t + .34;
            return false;
        }
        bool edge = false;
        if (sampled && second == lastSecond + 1 && t > lastTime) {
            const double width = t - lastTime + (rtt + lastRtt) / 2;
            const double center = (lastTime + t) / 2 + (rtt - lastRtt) / 4;
            if (width <= .65 && (!fitted || edges.size() < 4 ||
                                 std::abs(timeAt(second) - center) < std::max(.15, window * 2))) {
                edges.push_back({center, second, width});
                if (edges.size() > 120)
                    edges.pop_front();
                window = std::clamp(width * .4, .008, .25);
                fit();
                edge = true;
            } else
                ++rejected;
        }
        if (sampled && (second < lastSecond || second - lastSecond > (t - lastTime) * 1.01 + 2)) {
            // Unannounced discontinuity: don't bridge edges across it.
            edges.clear();
            inliers.clear();
            fitted = false;
            window = .2;
        }
        sampled = true;
        lastTime = t;
        lastSecond = second;
        lastRtt = rtt;
        if (edge) {
            phase = 0;
            due = timeAt(second + 1) - window;
        } else if (fitted || primed) {
            const double prediction = timeAt(second + 1);
            if (phase == 0 && t < prediction) {
                phase = 1;
                due = prediction;
            } else if (phase == 1 && t < prediction + window) {
                phase = 2;
                due = prediction + window;
            } else {
                phase = 0;
                window = std::min(.25, window * 1.5);
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
                v.push_back(1000 * std::abs(e.second - (intercept + slope * (e.time - origin))) / slope);
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
    double rate(double t) {
        prune(t);
        return requests.size() / 10.;
    }
};
inline bool eligible(bool enabled, bool coordinator, bool playing, bool ours, bool allowed) {
    return enabled && coordinator && playing && ours && allowed;
}
} // namespace timing_probe
