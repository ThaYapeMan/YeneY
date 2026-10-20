// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "playback_mode.h"
#include <cassert>
#include <iostream>
int main() {
    upnp::Speaker room{"127.0.0.1", "RINCON_TEST", "Study", "Study", {"Study"}};
    for (const char *value : {static_cast<const char *>(nullptr), "stream", "auto", "bad", "Queue", ""}) {
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
    room.members.push_back("Other");
    assert(bridge::selectPlaybackMode("queue", room).reason == "group-rate-undecided");
    room.coordinator = "Other";
    assert(bridge::selectPlaybackMode("queue", room).selected == bridge::PlaybackMode::Stream);
    room.members = {"Study"};
    assert(bridge::selectPlaybackMode("queue", room).reason == "group-rate-undecided");
    room.coordinator = "Study";
    room.members.clear();
    assert(bridge::selectPlaybackMode("queue", room).reason == "coordinator-unconfirmed");
    room.members = {"Study"};
    room.uuid.clear();
    assert(bridge::selectPlaybackMode("queue", room).selected == bridge::PlaybackMode::Stream);
    std::cout << "PASS playback mode policy: stream default, explicit standalone "
                 "queue, closed readiness flag, "
                 "deferred auto, invalid values, "
                 "grouped coordinators/members and unknown topology\n";
}
