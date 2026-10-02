#include "sonos-position.h"
#include "timing_probe_runtime.h"
#include <cassert>
#include <cstdio>
using namespace timing_probe;
int main() {
    setenv("YENEY_TIMING_PROBE", "1", 1);
    auto &d = diagnostics();
    reset_sonos_position(3);
    sonos_position_connection(3, 1);
    sonos_position_pcm(3, 1, 0);
    d.handed(3, 0, 44100);
    auto c = d.start(true, "fixture");
    const double now = clockSeconds();
    for (unsigned n = 1; n < 25; ++n) {
        d.response(c, true, now + n - .02, now + n - .01, n - 1);
        d.response(c, true, now + n + .01, now + n + .02, n);
    }
    double time, uncertainty;
    assert(d.estimate(44100 * 20, time, uncertainty));
    assert(std::abs(time - (now + 20)) < .005 && uncertainty < 25);
    size_t edges = 0;
    const auto before = d.snapshot(&edges);
    assert(edges > 10);
    // Gapless PCM uses the same existing anchor/token: no model reset.
    sonos_position_pcm(3, 1, 44100 * 20);
    auto gapless = d.snapshot();
    assert(gapless.epoch == before.epoch);
    d.handed(3, 44100 * 25, 44100);
    for (const char *reason : {"pause", "seek", "flush"}) {
        d.reset(reason, false, true);
        assert(d.snapshot().rate == 0);
        d.handed(3, 44100 * 25, 44100);
        assert(!d.estimate(0, time, uncertainty));
        c = d.start(true, "fixture");
        assert(c.epoch > gapless.epoch);
    }
    sonos_position_connection(3, 2);
    d.handed(3, 44100 * 25, 44100); // stale handoff cannot recreate an invalidated anchor
    assert(!d.start(true, "fixture").active);
    puts("PASS: cleared anchor rejects a late PCM handoff");
    auto reconnect = d.snapshot();
    assert(!reconnect.active && reconnect.rate == 0);
    sonos_position_pcm(3, 2, 44100 * 25);
    assert(d.snapshot().base == 44100 * 25);
    reset_sonos_position(4);
    sonos_position_connection(4, 3);
    sonos_position_pcm(4, 3, 0);
    assert(d.snapshot().stream == 4 && d.snapshot().base == 0);
    d.handed(4, 0, 48000);
    c = d.start(true, "fixture");
    // A reply from the old epoch can never seed the new model.
    d.response(before, true, now + 40, now + 40.01, 40);
    d.snapshot(&edges);
    assert(edges == 0);
    d.start(false, "fixture");
    assert(!d.snapshot().active);
    c = d.start(true, "fixture");
    const double budgetAt = clockSeconds();
    for (unsigned i = 0; i < 8; ++i)
        assert(d.request(c, budgetAt + i * .001));
    d.reset("budget-reset");
    c = d.start(true, "fixture");
    assert(!d.request(c, budgetAt + .01));
    c = d.start(true, "fixture");
    const double fresh = clockSeconds();
    for (unsigned n = 1; n < 12; ++n) {
        d.response(c, true, fresh + n - .02, fresh + n - .01, n - 1);
        d.response(c, true, fresh + n + .01, fresh + n + .02, n);
    }
    assert(!d.request(c, fresh + 22));
    assert(!d.snapshot().active);
    puts("PASS: ten seconds without usable edges expires the model instead of reporting false precision");
    puts("PASS: reset retains the rolling request budget");
    puts("PASS: actual position/PCM anchors reset on new stream/reconnect; stale replies rejected; "
         "pause/seek/flush reset; gapless PCM preserves epoch; frame mapping and uncertainty");
}
