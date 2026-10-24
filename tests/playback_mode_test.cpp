// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "playback_mode.h"
#include <cassert>
#include <iostream>
int main() {
    upnp::Speaker room{"127.0.0.1", "RINCON_TEST", "Study", "Study", {"Study"}};
    for (const char *value : {"stream", "auto", "bad", "Queue", ""}) {
        auto selection = bridge::selectPlaybackMode(value, room);
        assert(selection.selected == bridge::PlaybackMode::Stream);
        assert(selection.invalid ==
               (value && std::string(value) != "stream" && std::string(value) != "auto"));
    }
    assert(bridge::selectPlaybackMode("auto", room).reason == "auto-phase4");
    auto guarded = bridge::selectPlaybackMode("queue", room, false);
    assert(guarded.selected == bridge::PlaybackMode::Stream &&
           guarded.reason == "queue-unavailable");
    auto queued = bridge::selectPlaybackMode("queue", room);
    assert(queued.selected == bridge::PlaybackMode::Queue && queued.reason == "queue-opt-in");
    assert(bridge::selectPlaybackMode(nullptr, room).selected == bridge::PlaybackMode::Queue);
    assert(bridge::selectPlaybackMode(nullptr, room).reason == "default-queue");
    room.members.push_back("Other");
    assert(bridge::selectPlaybackMode("queue", room).selected == bridge::PlaybackMode::Queue);
    room.coordinator = "Other";
    assert(bridge::selectPlaybackMode("queue", room).selected == bridge::PlaybackMode::Queue);
    room.members = {"Study"};
    assert(bridge::selectPlaybackMode("queue", room).selected == bridge::PlaybackMode::Queue);
    room.coordinator = "Study";
    room.members.clear();
    assert(bridge::selectPlaybackMode("queue", room).selected == bridge::PlaybackMode::Queue);
    room.members = {"Study"};
    room.uuid.clear();
    assert(bridge::selectPlaybackMode("queue", room).selected == bridge::PlaybackMode::Stream);
    std::cout << "PASS playback mode policy: queue default, explicit stream, coordinator/member eligibility live, "
                 "queue, closed readiness flag, "
                 "deferred auto, invalid values, "
                 "grouped coordinators/members and unknown topology\n";
}
