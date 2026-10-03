// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "timing_brackets.h"
#include "timing_channel.h"
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
        const bool digits = *s && std::all_of(s, s + strlen(s), [](char c) { return c >= '0' && c <= '9'; });
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
} // namespace timing_probe
#include "timing_settings.h"
namespace timing_probe {
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
    BracketClock bracket;
    FrameMapping frames;
    Publisher publisher;
    double warmDrift = 0, lastPublish = -INFINITY, lastBracketAt = 0;
    unsigned publishedState = 0;
    double lockedSecond = NAN, lockedDue = 0;
    unsigned lockedPhase = 0, quickRetries = 0;
    std::deque<double> sendLatencies;
    double plannedSend = 0, burstHalf = .025, burstFirstSend = NAN, burstLastReceive = NAN;
    double burstFirstRtt = 0, burstLastRtt = 0;
    bool burstAllOld = true, burstAllNew = true;
    uint32_t discontinuity = 0;
    uint64_t audioGeneration = 0, audioCursor = 0;
    void retireFrames() {
        if (frames.valid)
            frames.valid_from = std::max(frames.valid_from, frames.generation == audioGeneration
                                                                ? audioCursor
                                                                : frames.abs_first + frames.frames);
    }
    void clearClock() {
        if (bracket.locked && bracket.driftReady)
            warmDrift = bracket.drift;
        retireFrames();
        bracket.reset(warmDrift);
        lastBracketAt = 0;
        started = nowClock();
        lockedSecond = NAN;
        lockedPhase = 0;
        quickRetries = 0;
    }
    void publish(unsigned state, bool immediate = false) {
        if (!publisher.active())
            return;
        const double now = nowClock();
        uint64_t absolute = 0;
        const uint64_t anchorFrame = std::max(context.base, frames.stream_first);
        if (state == 2 && (!frames.valid || frames.stream != context.stream || frames.rate != context.rate ||
                           !frames.absolute(anchorFrame, absolute)))
            state = 1;
        if (!immediate && state == publishedState && now - lastPublish < 1)
            return;
        TimingRecord r;
        r.model_epoch = context.epoch;
        r.state = state;
        r.discontinuity = discontinuity;
        r.shm_generation = frames.generation;
        r.updated_mono_ns = uint64_t(now * 1e9);
        r.offset_us = audibleOffset() * 1000;
        if (state == 2) {
            r.anchor_abs_frame = absolute;
            r.anchor_audible_mono_ns = uint64_t(
                (bracket.timeAt(double(anchorFrame - context.base) / context.rate) + audibleOffset() * .001) *
                1e9);
            r.sample_rate_hz = context.rate;
            r.drift_ppb = int32_t(std::llround(bracket.rateDrift() * 1000));
            r.uncertainty_us = uint32_t(std::ceil(bracket.uncertainty(now) * 1000));
            r.valid_from_abs_frame = std::max(absolute, frames.valid_from);
            r.valid_until_abs_frame = UINT64_MAX;
        }
        publisher.write(r);
        publishedState = state;
        lastPublish = now;
    }
    void scheduleLocked(double now) {
        if (!bracket.locked) {
            lockedSecond = NAN;
            return;
        }
        if (!std::isfinite(lockedSecond)) {
            lockedSecond =
                std::max(bracket.edges.back().second, model.lastSecond) + (quickRetries ? 1 : lockedEvery());
            lockedPhase = 0;
            burstFirstSend = burstLastReceive = NAN;
            burstAllOld = burstAllNew = true;
            const double r95 = percentile({model.rtts.begin(), model.rtts.end()}, .95);
            const double l95 = percentile({sendLatencies.begin(), sendLatencies.end()}, .95);
            burstHalf = std::min(.060, std::max(.020, (std::isfinite(r95) ? r95 : .020) +
                                                          (std::isfinite(l95) ? l95 : 0) + .005));
        }
        const double rtt = percentile({model.rtts.begin(), model.rtts.end()}, .5);
        const double correction = std::isfinite(rtt) ? rtt / 2 : .003;
        const double latency = percentile({sendLatencies.begin(), sendLatencies.end()}, .5);
        const double phase[] = {-burstHalf, 0, burstHalf};
        lockedDue = bracket.timeAt(lockedSecond) + phase[std::min(lockedPhase, 2u)] - correction -
                    (std::isfinite(latency) ? latency : 0);
        if (lockedDue < now && lockedPhase > 2)
            lockedDue = now;
    }

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
            "missed_edges=%u half_window_ms=%.3f state=%s model_epoch=%llu band_ms=%.3f "
            "violators=%u locked_drift_ppm=%.3f locked_rate_drift_ppm=%.3f "
            "published_uncertainty_ms=%.3f publish=%s one_sided_count=%u contradiction_count=%u "
            "last_two_sided_age_s=%.3f send_latency_p50_ms=%.3f burst_span_ms=%.3f%s reason=%s\n",
            room.c_str(), context.stream, context.rate, (unsigned long long)context.epoch,
            model.inliers.size(), percentile(widths, .5), percentile(residual, .5), percentile(residual, .95),
            uncertaintyMs, model.driftReported ? (model.slope - 1) * 1e6 : NAN, percentile(rtts, .5) * 1000,
            percentile(rtts, .95) * 1000, percentile(lead, .05), percentile(lead, .5),
            limiter.rate(nowClock()), model.rejected, errors,
            (unsigned long long)(context.base + uint64_t(std::max(0., model.lastSecond)) * context.rate),
            model.fitted ? model.timeAt(model.lastSecond) : NAN, mono, real, model.inliers.size(),
            model.outliers, model.edges.empty() ? NAN : double(model.outliers) / model.edges.size(),
            model.span, model.driftSigma, model.missed, model.window * 1000,
            bracket.locked ? "locked" : "acquiring", (unsigned long long)context.epoch, bracket.width * 1000,
            bracket.violators, bracket.driftReady && bracket.locked ? bracket.drift : NAN,
            bracket.driftReady && bracket.locked ? bracket.rateDrift() : NAN, bracket.uncertainty(nowClock()),
            publisher.active() ? "on" : "off", bracket.oneSided, bracket.contradictions,
            std::isfinite(bracket.lastTwoSided) ? std::max(0., nowClock() - bracket.lastTwoSided) : NAN,
            percentile({sendLatencies.begin(), sendLatencies.end()}, .5) * 1000, burstHalf * 2000,
            final ? " final" : "", reason);
    }

public:
    explicit Diagnostics(double (*clock)() = monotonicNow)
        : nowClock(clock), stale(enabled() ? staleSeconds() : 60), raw(rawEnabled()) {}
    bool configure(const std::string &mac) {
        if (!publishEnabled())
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (publisher.active() || !publisher.open(mac))
            return false;
        publish(0, true);
        return true;
    }
    void closePublication() {
        std::lock_guard<std::mutex> lock(mutex);
        publisher.close();
    }
    void exported(uint64_t generation, uint64_t cursor) {
        if (!publishEnabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (audioGeneration && generation != audioGeneration) {
            ++context.epoch;
            clearClock();
            frames.valid = false;
            frames.generation = generation;
            model.reset();
            leads.clear();
            discontinuity = 2;
            publish(1, true);
        }
        audioGeneration = generation;
        audioCursor = cursor;
    }
    void mapFrames(unsigned stream, unsigned rate, uint64_t generation, uint64_t first, uint64_t absolute,
                   uint64_t count, bool exported = true) {
        if (!publishEnabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (!exported) {
            frames.valid = false;
            ++context.epoch;
            clearClock();
            model.reset();
            started = nowClock();
            discontinuity = 2;
            publish(1, true);
            return;
        }
        if (frames.add(stream, rate, generation, first, absolute, count)) {
            ++context.epoch;
            discontinuity = 2;
            // Every new export run requires a new offset, even at the same rate.
            clearClock();
            model.reset();
            leads.clear();
            started = nowClock();
            nextLog = started + 10;
            publish(context.active ? 1 : 0, true);
        }
    }
    void reset(const char *reason, bool clearAnchor = false, bool invalidateRate = false) {
        if (!enabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        line(true, reason);
        ++context.epoch;
        context.active = false;
        model.reset();
        clearClock();
        leads.clear();
        errors = consecutiveFailures = 0;
        discontinuity = !strcmp(reason, "pause") ? 1 : 2;
        publish(0, true);
        if (!strcmp(reason, "shutdown"))
            publisher.close();
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
        clearClock();
        leads.clear();
        discontinuity = 2;
        publish(1, true);
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
            leads.push_back((model.timeAt(double(frame - context.base) / rate) - nowClock()) * 1000);
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
                clearClock();
                leads.clear();
                discontinuity = 2;
                publish(0, true);
            }
        } else if (!context.active) {
            context.active = true;
            errors = consecutiveFailures = 0;
            room = name;
            started = nowClock();
            nextLog = started + 10;
            discontinuity = 0;
            publish(1, true);
        }
        if (!allowed)
            publish(0);
        return context;
    }
    bool request(const Context &c, double now) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!context.active || c.epoch != context.epoch)
            return false;
        if (now - (lastBracketAt ? lastBracketAt : started) > stale) {
            line(true, "stale-edges");
            ++context.epoch;
            context.active = false;
            model.reset();
            clearClock();
            leads.clear();
            discontinuity = 2;
            publish(3, true);
            return false;
        }
        if (now >= nextLog) {
            line(false, "periodic");
            nextLog = now + 10;
        }
        publish(bracket.locked ? 2 : 1);
        scheduleLocked(now);
        const double due = bracket.locked ? lockedDue : model.due;
        if (now < due || now < backoff || !limiter.take(now))
            return false;
        plannedSend = bracket.locked ? lockedDue : now;
        pendingSequence = ++sequence;
        pendingKind = bracket.locked ? "burst" : !model.sampled ? "seed" : model.phase ? "burst" : "search";
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
    bool contract(uint64_t frame, double &time, double &uncertaintyMs, double *drift = nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!context.active || !context.rate || !bracket.locked || frame < context.base)
            return false;
        time = bracket.timeAt(double(frame - context.base) / context.rate);
        uncertaintyMs = bracket.uncertainty(nowClock());
        if (drift)
            *drift = bracket.drift;
        return true;
    }
    void response(const Context &c, bool ok, double sent, double received, double second,
                  const std::string &relTime = "", const char *failure = "error",
                  const std::string &failureReason = "soap-or-parse") {
        std::lock_guard<std::mutex> lock(mutex);
        if (pendingSequence && std::isfinite(sent))
            limiter.sent(sent + .005); // Allow for receive-side timestamp/dispatch jitter.
        const bool requested = pendingSequence != 0;
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
            const bool wasLocked = bracket.locked;
            if (wasLocked && requested && lockedPhase == 0) {
                sendLatencies.push_back(std::max(0., sent - plannedSend));
                if (sendLatencies.size() > 120)
                    sendLatencies.pop_front();
            }
            if (wasLocked && requested && !strcmp(outcome, "accepted")) {
                if (!std::isfinite(burstFirstSend)) {
                    burstFirstSend = sent;
                    burstFirstRtt = received - sent;
                }
                burstLastRtt = received - sent;
                burstLastReceive = received;
                burstAllOld = burstAllOld && second < lockedSecond;
                burstAllNew = burstAllNew && second >= lockedSecond;
            }
            if (model.decision.observed) {
                const auto e = model.decision.edge;
                if (bracket.add({e.second, e.time - e.width / 2, e.time + e.width / 2}))
                    lastBracketAt = e.time;
                if (bracket.locked && bracket.driftReady)
                    warmDrift = bracket.drift;
                if (wasLocked && e.second >= lockedSecond)
                    lockedSecond = NAN;
                if (wasLocked && !std::isfinite(lockedSecond))
                    quickRetries = 0;
            }
            if (wasLocked && requested && std::isfinite(lockedSecond)) {
                if (++lockedPhase >= 3) {
                    if ((burstAllOld || burstAllNew) && std::isfinite(burstFirstSend)) {
                        const Bracket bound{lockedSecond, burstAllNew ? -INFINITY : burstLastReceive,
                                            burstAllNew ? burstFirstSend : INFINITY,
                                            burstAllNew ? burstFirstRtt : burstLastRtt};
                        if (bracket.add(bound))
                            lastBracketAt = received;
                        if (raw)
                            printf(
                                "yeney: timing-edge room=%s stream=%u epoch=%llu seq=%llu second=%.0f->%.0f "
                                "lo_mono=%.9f hi_mono=%.9f mid_mono=nan half_width_ms=nan residual_ms=nan "
                                "classification=%s rule=one-sided-send-receive\n",
                                quoted(room).c_str(), c.stream, (unsigned long long)c.epoch,
                                (unsigned long long)seq, bound.second - 1, bound.second, bound.lo, bound.hi,
                                burstAllNew ? "upper-bound" : "lower-bound");
                    }
                    quickRetries = quickRetries < 3 ? quickRetries + 1 : 0;
                    lockedSecond = NAN;
                }
            }
            if (wasLocked != bracket.locked) {
                if (wasLocked) {
                    ++context.epoch;
                    retireFrames();
                }
                publish(bracket.locked ? 2 : 1, true);
            }
        }
        if (raw) {
            printf("yeney: timing-raw room=%s stream=%u epoch=%llu seq=%llu kind=%s "
                   "send_mono=%.9f recv_mono=%.9f rtt_ms=%.3f reltime=%s reltime_s=%.3f "
                   "outcome=%s reason=%s\n",
                   quoted(room).c_str(), c.stream, (unsigned long long)c.epoch, (unsigned long long)seq,
                   pendingKind, sent, received, (received - sent) * 1000, quoted(relTime).c_str(), second,
                   outcome, quoted(reason).c_str());
            if (current && ok && model.decision.observed) {
                const auto &d = model.decision;
                printf("yeney: timing-edge room=%s stream=%u epoch=%llu seq=%llu second=%.0f->%.0f "
                       "lo_mono=%.9f hi_mono=%.9f mid_mono=%.9f half_width_ms=%.3f "
                       "residual_ms=%.3f classification=%s rule=%s\n",
                       quoted(room).c_str(), c.stream, (unsigned long long)c.epoch, (unsigned long long)seq,
                       d.edge.second - 1, d.edge.second, d.edge.time - d.edge.width / 2,
                       d.edge.time + d.edge.width / 2, d.edge.time, d.edge.width * 500, d.residual,
                       d.inlier ? "inlier" : "outlier", d.rule);
            }
        }
    }
};
inline Diagnostics &diagnostics() {
    static Diagnostics d;
    return d;
}
} // namespace timing_probe
