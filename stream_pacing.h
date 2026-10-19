// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
namespace bridge {
inline bool streamPacingSonos() {
    static const bool sonos = [] {
        const char *s = std::getenv("YENEY_STREAM_PACING");
        if (s && std::strcmp(s,"wall") && std::strcmp(s,"sonos"))
            std::fprintf(stderr,"YENEY_STREAM_PACING invalid; using wall (valid: wall|sonos)\n");
        return s && !std::strcmp(s,"sonos");
    }();
    return sonos;
}
struct StreamClock { double playedMs = 0, scale = 1; bool locked = false; };
// Monotonic encoder-local clock. A model phase correction is slewed at <=1%
// of wall time; rate changes preserve the current phase. Neither acquisition,
// prior replacement, loss of lock nor pause creates a jump in admission credit.
class StreamPacer {
    double played = 0;
    uint64_t previous = 0, readAnchor = 0;
public:
    double update(uint64_t now, uint64_t firstRead, StreamClock clock, bool paused) {
        if (!firstRead) { previous = now; return played; }
        if (!std::isfinite(clock.scale) || clock.scale < .999 || clock.scale > 1.001) clock.scale = 1;
        if (!readAnchor) { readAnchor = firstRead; previous = firstRead; }
        double dt = now >= previous ? double(now-previous) : 0;
        previous = now;
        if (paused) return played;
        double next = played + dt*clock.scale;
        if (clock.locked && std::isfinite(clock.playedMs) && clock.playedMs >= 0)
            next += std::clamp(clock.playedMs-next, -.01*dt, .01*dt);
        played = std::max(played,next);
        return played;
    }
    static double ceiling(double played, unsigned startLead) { return played + startLead + 500.; }
};
}
