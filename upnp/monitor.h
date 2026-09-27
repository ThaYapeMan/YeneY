#pragma once
#include "speaker_state.h"
#include "timing.h"
#include <functional>
namespace upnp {
struct MonitorWork { bool transport = false, media = false, mediaDue = false, volume = false, topology = false, position = false; };
// Pure policy: supplied time is the only clock; no sockets, threads or bridge state.
class MonitorPolicy {
    std::array<StateClock::time_point, 5> due{};
    std::array<bool, 3> previous{};
    bool initialized = false;
public:
    bool legacy = false;
    bool stoppedMediaInfo = false;
    MonitorWork next(StateClock::time_point now, const SpeakerState& state,
                     const std::array<bool, 3>& active, bool requestOpen = false) {
        if (!initialized) { due.fill(now); initialized = true; }
        // Recovery immediately stops fallback. Loss immediately enables that service.
        for (size_t i = 0; i < active.size(); ++i) if (active[i] != previous[i]) {
            if (i == 0) { due[0] = active[i] && !legacy ? now + timing::sanity : now; due[1] = now; }
            else due[i + 1] = now;
        }
        previous = active;
        MonitorWork work;
        auto ready = [&](size_t index, auto interval) {
            if (now < due[index]) return false;
            due[index] = now + interval; return true;
        };
        if (legacy || !active[0]) {
            work.transport = legacy || ready(0, std::chrono::milliseconds(timing::pollMs));
            work.mediaDue = ready(1, timing::position);
            work.media = work.mediaDue && (!state.paused() || (stoppedMediaInfo && !requestOpen));
        } else work.transport = ready(0, timing::sanity);
        work.volume = (legacy || !active[1]) && ready(2, timing::position);
        work.topology = (legacy || !active[2]) && ready(3, timing::topology);
        work.position = (legacy || state.playing()) && ready(4, timing::position);
        return work;
    }
};
}
