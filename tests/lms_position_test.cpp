// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "position_state.h"
#include "timing_probe_runtime.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

static double now = 100;
static double clockNow() { return now; }
static constexpr unsigned rate = 48000;
static constexpr double speed = 1.000012;
static constexpr double k = 1 / speed;
static void edge(timing_probe::Diagnostics &d, unsigned second) {
    auto c = d.snapshot();
    const double saved = now, t = 100 + second * k;
    now = t - .001;
    d.response(c, true, now - .002, now + .002, second - 1);
    now = t + .001;
    d.response(c, true, now - .002, now + .002, second);
    now = saved;
}
int main(int argc, char **) {
    if (argc > 1) {
        printf("model=%d\n", timing_probe::lmsPositionFromModel());
        return 0;
    }
    setenv("YENEY_TIMING_PROBE", "1", 1);
    timing_probe::Diagnostics d(clockNow);
    ConnectionPosition p;
    p.connection(1, 1, 100000);
    p.pcm(1, 1, 0, 100000);
    d.anchor(1, 0);
    d.handed(1, 0, rate);
    d.start(true, "model-position");
    double frame, scale;
    assert(!d.lmsPosition(1, 0, rate, frame, scale));
    p.poll(p.token(), 2000, 102600);
    assert(p.audibleFrames(rate) == 2 * rate);
    puts("PASS: legacy RelTime fallback remains available while acquiring");
    for (unsigned n = 1; n <= 240; ++n)
        edge(d, n);
    unsigned lastEdge = 240;
    uint64_t previous = 0, first = 0, last = 0;
    double maxError = 0;
    const double start = 100 + 240 * k + .4;
    double nextPoll = start, lastSampleTime = 0, lastSampleSecond = 0;
    for (unsigned i = 0; i <= 240; ++i) {
        now = start + i * .25;
        const unsigned second = unsigned((now - 100) * speed);
        if (second > lastEdge) {
            edge(d, second);
            lastEdge = second;
        }
        // The lease phase slips through the tick boundary over the run.
        if (now >= nextPoll) {
            // The status consumer can expose a cached SOAP value half a second later.
            lastSampleTime = now - .5;
            lastSampleSecond = std::floor((lastSampleTime - 100) * speed);
            d.seed(d.snapshot(), lastSampleSecond, lastSampleTime);
            p.poll(p.token(), unsigned(lastSampleSecond) * 1000, uint64_t(now * 1000));
            nextPoll += 1.007;
        }
        double relSecond, observed;
        assert(d.lmsPosition(1, 0, rate, frame, scale, &relSecond, &observed));
        uint64_t selected;
        assert(p.modelAudibleFrames(rate, uint64_t(now * 1000), frame, scale, selected, relSecond, observed));
        assert(selected >= previous);
        const double truth = (now - 100) * speed * rate;
        maxError = std::max(maxError, std::abs(double(selected) - truth) * 1000 / rate);
        previous = selected;
        if (!i)
            first = selected;
        last = selected;
    }
    const double ppm = (double(last - first) / (60 * rate) - 1) * 1e6;
    assert(maxError <= 5 && std::abs(ppm - 12) < 1);
    printf(
        "PASS: 60 s drifting RelTime lease: maximum model error %.3f ms, rate %.3f ppm, no backward steps\n",
        maxError, ppm);
    assert(!d.lmsPosition(2, 0, rate, frame, scale));
    assert(!d.lmsPosition(1, 1, rate, frame, scale));
    assert(!d.lmsPosition(1, 0, 44100, frame, scale));
    for (const char *reason : {"pause", "seek", "flush", "reconnect", "stream-reset"}) {
        d.reset(reason, false, true);
        assert(!d.lmsPosition(1, 0, rate, frame, scale));
        // A lagging floored fallback must not undo a previously modelled frame.
        assert(p.audibleFrames(rate) >= previous);
        d.anchor(1, 0);
        d.handed(1, 0, rate);
        d.start(true, "relock");
        assert(!d.lmsPosition(1, 0, rate, frame, scale));
        for (unsigned n = 1; n <= lastEdge; ++n)
            edge(d, n);
        assert(d.lmsPosition(1, 0, rate, frame, scale));
        uint64_t selected;
        assert(p.modelAudibleFrames(rate, uint64_t(now * 1000), frame, scale, selected));
        assert(selected >= previous);
        previous = selected;
    }
    puts("PASS: pause/seek/flush/reconnect/new-stream reset to fallback until re-lock; switches never "
         "decrease");
    now += 61;
    assert(!d.lmsPosition(1, 0, rate, frame, scale));
    puts("PASS: stale model rejected; stream/base/rate mismatches rejected");
}
