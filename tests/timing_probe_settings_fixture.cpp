#include "timing_probe_runtime.h"
#include <cassert>
#include <cstdio>
using namespace timing_probe;
static double now = 10000;
static double fakeClock() { return now; }
int main(int argc, char **argv) {
    Diagnostics d(fakeClock);
    printf("settings probe=%d raw=%d stale=%u\n", enabled(), rawEnabled(),
           enabled() ? staleSeconds() : 60);
    if (argc > 1 && std::string(argv[1]) == "settings") {
        if (enabled()) {
            (void)rawEnabled();
            (void)staleSeconds();
        }
        return 0;
    }
    if (!enabled())
        return 0;
    d.anchor(3, 0);
    d.handed(3, 0, 44100);
    auto c = d.start(true, "test room");
    assert(d.request(c, now));
    for (unsigned n = 1; n <= 25; ++n) {
        now = 10000 + n;
        d.response(c, true, now - .020, now - .010, n - 1, "0:00:24");
        d.response(c, true, now + .010, now + .020, n, "0:00:25");
        if (n == 19) {
            double mono, uncertainty;
            assert(d.estimate(44100 * 19, mono, uncertainty));
            assert(std::isnan(uncertainty));
        }
    }
    double mono, uncertainty;
    assert(d.estimate(44100 * 25, mono, uncertainty));
    assert(std::isfinite(uncertainty));
    const auto epoch = c.epoch;
    now += std::min(30u, staleSeconds() - 1);
    (void)d.request(c, now);
    assert(d.snapshot().epoch == epoch && d.snapshot().active);
    assert(d.estimate(44100 * 25, mono, uncertainty));
    puts("PASS: fitted clock retained through the permitted gap");
    d.response(c, true, now, now + .100, 55, "0:00:55");
    d.response(c, false, now, now + .200, NAN, "", "timeout", "timeout");
    d.response(c, false, now, now + .010, NAN, "", "error", "soap-error");
    now = 10025 + staleSeconds() + 1;
    assert(!d.request(c, now));
    assert(!d.snapshot().active && d.snapshot().epoch > epoch);
    d.response(c, true, now, now + .010, 100, "0:01:40");
    puts("PASS: configured stale gap expires once; late reply fenced");
}
