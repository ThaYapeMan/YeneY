// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "timing_probe.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <time.h>
namespace timing_probe {
inline double clockSeconds(clockid_t clock = CLOCK_MONOTONIC) {
    timespec t{};
    clock_gettime(clock, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
inline bool enabled() {
    static const bool on = [] {
        const char *s = getenv("YENEY_TIMING_PROBE");
        if (s && strcmp(s, "0") && strcmp(s, "1"))
            printf("yeney: setting key=YENEY_TIMING_PROBE invalid=%s fallback=0\n", s);
        return s && !strcmp(s, "1");
    }();
    return on;
}
struct Context {
    unsigned stream = 0, rate = 0;
    uint64_t epoch = 0, base = 0;
    bool active = false, anchored = false;
};
class Diagnostics {
    std::mutex mutex;
    Context context;
    Estimator model;
    Limiter limiter; // Deliberately survives every reset/backoff.
    std::deque<double> leads;
    double nextLog = 0, backoff = 0;
    unsigned errors = 0, consecutiveFailures = 0;
    std::string room;
    void line(bool final, const char *reason) {
        if (!context.active)
            return;
        std::vector<double> widths, rtts(model.rtts.begin(), model.rtts.end()),
            lead(leads.begin(), leads.end());
        for (auto e : model.inliers)
            widths.push_back(e.width * 1000);
        auto residual = model.residuals();
        const double mono = clockSeconds(), real = clockSeconds(CLOCK_REALTIME);
        const double uncertainty = percentile(widths, .95) / 2 + percentile(residual, .95);
        printf(
            "yeney: timing room=%s stream=%u rate=%u epoch=%llu edges=%zu window_ms=%.3f "
            "residual_p50_ms=%.3f residual_p95_ms=%.3f uncertainty_ms=%.3f drift_ppm=%.3f rtt_p50_ms=%.3f "
            "rtt_p95_ms=%.3f lead_p5_ms=%.3f lead_p50_ms=%.3f probe_rps=%.2f rejected=%u errors=%u "
            "frame=%llu audible_mono=%.9f mono=%.9f real=%.9f%s reason=%s\n",
            room.c_str(), context.stream, context.rate, (unsigned long long)context.epoch,
            model.inliers.size(), percentile(widths, .5), percentile(residual, .5), percentile(residual, .95),
            uncertainty, model.inliers.size() >= 10 ? (model.slope - 1) * 1e6 : NAN,
            percentile(rtts, .5) * 1000, percentile(rtts, .95) * 1000, percentile(lead, .05),
            percentile(lead, .5), limiter.rate(mono), model.rejected, errors,
            (unsigned long long)(context.base + uint64_t(std::max(0., model.lastSecond)) * context.rate),
            model.fitted ? model.timeAt(model.lastSecond) : NAN, mono, real, final ? " final" : "", reason);
    }

  public:
    void reset(const char *reason, bool clearAnchor = false, bool invalidateRate = false) {
        if (!enabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        line(true, reason);
        ++context.epoch;
        context.active = false;
        model.reset();
        leads.clear();
        errors = consecutiveFailures = 0;
        if (clearAnchor) {
            context.rate = 0;
            context.base = 0;
            context.anchored = false;
        } else if (invalidateRate)
            context.rate = 0;
    }
    void anchor(unsigned stream, uint64_t base) {
        if (!enabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        line(true, "pcm-anchor");
        ++context.epoch;
        context.stream = stream;
        context.base = base;
        context.anchored = true;
        context.active = false;
        model.reset();
        leads.clear();
    }
    void handed(unsigned stream, uint64_t frame, unsigned rate) {
        if (!enabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (stream != context.stream || !context.anchored)
            return;
        context.rate = rate;
        if (context.active && model.fitted && !model.edges.empty() &&
            clockSeconds() - model.edges.back().time <= 10 && frame >= context.base) {
            leads.push_back((model.timeAt(double(frame - context.base) / rate) - clockSeconds()) * 1000);
            if (leads.size() > 4096)
                leads.pop_front();
        }
    }
    Context start(bool allowed, const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!allowed || !context.rate || !context.anchored) {
            if (context.active) {
                line(true, "ineligible");
                ++context.epoch;
                context.active = false;
                model.reset();
                leads.clear();
            }
        } else if (!context.active) {
            context.active = true;
            errors = consecutiveFailures = 0;
            room = name;
            nextLog = clockSeconds() + 10;
        }
        return context;
    }
    bool request(const Context &c, double now) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!context.active || c.epoch != context.epoch)
            return false;
        if (model.fitted && !model.edges.empty() && now - model.edges.back().time > 10) {
            line(true, "stale-edges");
            ++context.epoch;
            context.active = false;
            model.reset();
            leads.clear();
            return false;
        }
        if (now >= nextLog) {
            line(false, "periodic");
            nextLog = now + 10;
        }
        return now >= model.due && now >= backoff && limiter.take(now);
    }
    void seed(const Context &c, double second, double time) {
        std::lock_guard<std::mutex> lock(mutex);
        if (c.active && context.active && c.epoch == context.epoch && !model.sampled) {
            // Only guide acquisition: cached/coarse reads are never fitted edges.
            model.seed(second, time);
        }
    }
    Context snapshot(size_t *edges = nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        if (edges)
            *edges = model.inliers.size();
        return context;
    }
    bool estimate(uint64_t frame, double &monotonic, double &uncertaintyMs) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!context.active || !context.rate || !model.fitted || model.edges.empty() ||
            clockSeconds() - model.edges.back().time > 10 || frame < context.base)
            return false;
        monotonic = model.timeAt(double(frame - context.base) / context.rate);
        std::vector<double> widths;
        for (auto e : model.inliers)
            widths.push_back(e.width * 1000);
        uncertaintyMs = percentile(widths, .95) / 2 + percentile(model.residuals(), .95);
        return true;
    }
    void response(const Context &c, bool ok, double sent, double received, double second) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!context.active || c.epoch != context.epoch)
            return;
        if (!ok) {
            ++errors;
            ++consecutiveFailures;
            backoff = clockSeconds() + std::min(10., std::pow(2., std::min(consecutiveFailures, 4u)));
            return;
        }
        backoff = 0;
        consecutiveFailures = 0;
        model.sample(sent, received, second);
    }
};
inline Diagnostics &diagnostics() {
    static Diagnostics d;
    return d;
}
} // namespace timing_probe
