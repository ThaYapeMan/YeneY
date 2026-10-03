// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "timing_probe.h"
#include <cerrno>
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
// Extra settings are deliberately not inspected while the probe is disabled.
inline bool rawEnabled() {
    if (!enabled())
        return false;
    static const bool on = [] {
        const char *s = getenv("YENEY_TIMING_RAW");
        if (s && strcmp(s, "0") && strcmp(s, "1"))
            printf("yeney: setting key=YENEY_TIMING_RAW invalid=%s fallback=0\n", s);
        return s && !strcmp(s, "1");
    }();
    return on;
}
inline unsigned staleSeconds() {
    static const unsigned seconds = [] {
        const char *s = getenv("YENEY_TIMING_STALE_S");
        if (!s)
            return 60u;
        char *end = nullptr;
        errno = 0;
        const unsigned long n = strtoul(s, &end, 10);
        const bool digits =
            *s && std::all_of(s, s + strlen(s), [](char c) { return c >= '0' && c <= '9'; });
        if (errno || !digits || *end || n < 10 || n > 600) {
            printf("yeney: setting key=YENEY_TIMING_STALE_S invalid=%s fallback=60\n", s);
            return 60u;
        }
        return unsigned(n);
    }();
    return seconds;
}
inline std::string quoted(const std::string &text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') {
            result += '\\';
            result += char(c);
        } else if (c < 32 || c == 127) {
            char escape[5];
            snprintf(escape, sizeof escape, "\\x%02x", c);
            result += escape;
        } else
            result += char(c);
    }
    return result + "\"";
}
inline double monotonicNow() { return clockSeconds(); }
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
    double (*nowClock)();
    double started = 0;
    uint64_t sequence = 0, pendingSequence = 0;
    const char *pendingKind = "seed";
    unsigned stale;
    bool raw;
    double uncertainty() const {
        if (model.inliers.size() < 20)
            return NAN;
        std::vector<double> widths;
        for (auto e : model.inliers)
            widths.push_back(e.width * 1000);
        return percentile(widths, .95) / 2 + percentile(model.residuals(), .95);
    }
    void line(bool final, const char *reason) {
        if (!context.active)
            return;
        std::vector<double> widths, rtts(model.rtts.begin(), model.rtts.end()),
            lead(leads.begin(), leads.end());
        for (auto e : model.inliers)
            widths.push_back(e.width * 1000);
        auto residual = model.residuals();
        const double mono = clockSeconds(), real = clockSeconds(CLOCK_REALTIME);
        const double uncertaintyMs = uncertainty();
        printf(
            "yeney: timing room=%s stream=%u rate=%u epoch=%llu edges=%zu window_ms=%.3f "
            "residual_p50_ms=%.3f residual_p95_ms=%.3f uncertainty_ms=%.3f drift_ppm=%.3f "
            "rtt_p50_ms=%.3f "
            "rtt_p95_ms=%.3f lead_p5_ms=%.3f lead_p50_ms=%.3f probe_rps=%.2f rejected=%u errors=%u "
            "frame=%llu audible_mono=%.9f mono=%.9f real=%.9f "
            "inliers=%zu outliers=%u outlier_frac=%.4f span_s=%.3f drift_sigma_ppm=%.3f "
            "missed_edges=%u half_window_ms=%.3f%s reason=%s\n",
            room.c_str(), context.stream, context.rate, (unsigned long long)context.epoch,
            model.inliers.size(), percentile(widths, .5), percentile(residual, .5),
            percentile(residual, .95), uncertaintyMs,
            model.driftReported ? (model.slope - 1) * 1e6 : NAN, percentile(rtts, .5) * 1000,
            percentile(rtts, .95) * 1000, percentile(lead, .05), percentile(lead, .5),
            limiter.rate(nowClock()), model.rejected, errors,
            (unsigned long long)(context.base +
                                 uint64_t(std::max(0., model.lastSecond)) * context.rate),
            model.fitted ? model.timeAt(model.lastSecond) : NAN, mono, real, model.inliers.size(),
            model.outliers, model.edges.empty() ? NAN : double(model.outliers) / model.edges.size(),
            model.span, model.driftSigma, model.missed, model.window * 1000, final ? " final" : "",
            reason);
    }

  public:
    explicit Diagnostics(double (*clock)() = monotonicNow)
        : nowClock(clock), stale(enabled() ? staleSeconds() : 60), raw(rawEnabled()) {}
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
            nowClock() - model.lastUsable <= stale && frame >= context.base) {
            leads.push_back((model.timeAt(double(frame - context.base) / rate) - nowClock()) *
                            1000);
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
            started = nowClock();
            nextLog = started + 10;
        }
        return context;
    }
    bool request(const Context &c, double now) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!context.active || c.epoch != context.epoch)
            return false;
        if (model.stale(now, started, stale)) {
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
        if (now < model.due || now < backoff || !limiter.take(now))
            return false;
        pendingSequence = ++sequence;
        pendingKind = !model.sampled ? "seed" : model.phase ? "burst" : "search";
        return true;
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
            nowClock() - model.lastUsable > stale || frame < context.base)
            return false;
        monotonic = model.timeAt(double(frame - context.base) / context.rate);
        uncertaintyMs = uncertainty();
        return true;
    }
    void response(const Context &c, bool ok, double sent, double received, double second,
                  const std::string &relTime = "", const char *failure = "error",
                  const std::string &failureReason = "soap-or-parse") {
        std::lock_guard<std::mutex> lock(mutex);
        if (pendingSequence && std::isfinite(sent))
            limiter.sent(sent + .005); // Allow for receive-side timestamp/dispatch jitter.
        const uint64_t seq = pendingSequence ? pendingSequence : ++sequence;
        pendingSequence = 0;
        const char *outcome = "accepted";
        std::string reason = "sample";
        bool current = context.active && c.epoch == context.epoch;
        if (!current) {
            outcome = "rejected-stale-epoch";
            reason = "epoch-or-eligibility-changed";
        } else if (!ok) {
            outcome = failure;
            reason = failureReason;
            ++errors;
            ++consecutiveFailures;
            backoff = nowClock() + std::min(10., std::pow(2., std::min(consecutiveFailures, 4u)));
        } else {
            backoff = 0;
            consecutiveFailures = 0;
            model.sample(sent, received, second);
            outcome = model.sampleOutcome;
            reason = model.sampleReason;
        }
        if (raw) {
            printf("yeney: timing-raw room=%s stream=%u epoch=%llu seq=%llu kind=%s "
                   "send_mono=%.9f recv_mono=%.9f rtt_ms=%.3f reltime=%s reltime_s=%.3f "
                   "outcome=%s reason=%s\n",
                   quoted(room).c_str(), c.stream, (unsigned long long)c.epoch,
                   (unsigned long long)seq, pendingKind, sent, received, (received - sent) * 1000,
                   quoted(relTime).c_str(), second, outcome, quoted(reason).c_str());
            if (current && ok && model.decision.observed) {
                const auto &d = model.decision;
                printf("yeney: timing-edge room=%s stream=%u epoch=%llu seq=%llu second=%.0f->%.0f "
                       "lo_mono=%.9f hi_mono=%.9f mid_mono=%.9f half_width_ms=%.3f "
                       "residual_ms=%.3f classification=%s rule=%s\n",
                       quoted(room).c_str(), c.stream, (unsigned long long)c.epoch,
                       (unsigned long long)seq, d.edge.second - 1, d.edge.second,
                       d.edge.time - d.edge.width / 2, d.edge.time + d.edge.width / 2, d.edge.time,
                       d.edge.width * 500, d.residual, d.inlier ? "inlier" : "outlier", d.rule);
            }
        }
    }
};
inline Diagnostics &diagnostics() {
    static Diagnostics d;
    return d;
}
} // namespace timing_probe
