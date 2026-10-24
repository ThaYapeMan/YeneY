// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "upnp/speaker_control.h"
#include <string>
namespace bridge {
// Source-aware queue playback is the built-in default. Eligibility is live.
inline constexpr bool queuePlaybackReady = true;
enum class PlaybackMode { Stream, Queue };
struct PlaybackSelection {
    std::string requested, reason;
    PlaybackMode selected = PlaybackMode::Stream;
    bool invalid = false;
};
// The caller reads the room environment once, after topology discovery. No I/O
// or cached global setting here: topology and configuration remain explicit.
inline PlaybackSelection selectPlaybackMode(const char *value, const upnp::Speaker &speaker,
                                            bool ready = queuePlaybackReady) {
    PlaybackSelection result;
    result.requested = value ? value : "queue";
    if (result.requested == "stream") {
        result.reason = "configured-stream";
    } else if (result.requested == "auto") {
        result.reason = "auto-phase4";
    } else if (result.requested != "queue") {
        result.invalid = true;
        result.reason = "invalid-setting";
    } else if (speaker.uuid.empty() || speaker.ip.empty() || speaker.name.empty()) {
        result.reason = "coordinator-unconfirmed";
    } else if (!ready) {
        result.reason = "queue-unavailable";
    } else {
        result.selected = PlaybackMode::Queue;
        result.reason = value ? "queue-opt-in" : "default-queue";
    }
    return result;
}
inline const char *playbackModeName(PlaybackMode mode) {
    return mode == PlaybackMode::Queue ? "queue" : "stream";
}
} // namespace bridge
