// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "upnp/speaker_control.h"
#include <string>
namespace bridge {
enum class PlaybackMode { Stream, Queue };
struct PlaybackSelection {
    std::string requested, reason;
    PlaybackMode selected = PlaybackMode::Stream;
    bool invalid = false;
};
// The caller reads the room environment once, after topology discovery. No I/O
// or cached global setting here: topology and configuration remain explicit.
inline PlaybackSelection selectPlaybackMode(const char *value, const upnp::Speaker &speaker) {
    PlaybackSelection result;
    result.requested = value ? value : "stream";
    if (result.requested == "stream") {
        result.reason = value ? "configured-stream" : "default-stream";
    } else if (result.requested == "auto") {
        result.reason = "auto-phase4";
    } else if (result.requested != "queue") {
        result.invalid = true;
        result.reason = "invalid-setting";
    } else if (speaker.members.size() > 1 ||
               (!speaker.coordinator.empty() && speaker.coordinator != speaker.name)) {
        result.reason = "group-rate-undecided";
    } else if (speaker.uuid.empty() || speaker.ip.empty() || speaker.name.empty() ||
               speaker.coordinator != speaker.name || speaker.members.size() != 1 ||
               speaker.members.front() != speaker.name) {
        result.reason = "coordinator-unconfirmed";
    } else {
        result.selected = PlaybackMode::Queue;
        result.reason = "queue-opt-in";
    }
    return result;
}
inline const char *playbackModeName(PlaybackMode mode) {
    return mode == PlaybackMode::Queue ? "queue" : "stream";
}
} // namespace bridge
