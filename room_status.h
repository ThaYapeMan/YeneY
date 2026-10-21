// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "playback_mode.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>
namespace bridge {
struct ModeEvidence {
    uint64_t generation = 0, epoch = 0, occurrence = 0, token = 0, frame = 0, promised = 0, actual = 0;
    unsigned rate = 0, index = 0, sourceMs = 0, ahead = 0;
    bool owned = false, playing = false, ended = false, usable = true;
};
struct ModeStatus {
    std::string requested = "stream", active = "stream", state = "active", reason = "default-stream",
                lastFallback;
    std::vector<std::string> ownedUris;
    std::string currentUri, lastFault;
    ModeEvidence item;
    uint64_t generation = 1;
    unsigned oldRate = 0, newRate = 0;
    bool latched = false;
    uint64_t lmsSlow = 0, lmsFailed = 0;
    double gapMs = -1;
};
// Passive status only. The relay publishes before returning from each observation.
class RoomMode {
    mutable std::mutex mutex;
    ModeStatus value;

public:
    ModeStatus status() const {
        std::lock_guard<std::mutex> lock(mutex);
        return value;
    }
    void publish(ModeStatus v) {
        std::lock_guard<std::mutex> lock(mutex);
        value = std::move(v);
    }
    static std::string json(const ModeStatus &);
};
inline std::string RoomMode::json(const ModeStatus &v) {
    // Values are fixed reason codes/config selections; escape arbitrary input.
    auto q = [](const std::string &t) {
        std::string o = "\"";
        for (unsigned char c : t) {
            if (c == '"' || c == '\\')
                o += '\\';
            if (c >= 32)
                o += char(c);
            else
                o += '?';
        }
        return o + '"';
    };
    if (v.active == "queue") {
        std::ostringstream out;
        out << "{\"requested\":\"queue\",\"active\":\"queue\",\"reason\":\"queue-opt-in\",\"state\":"
            << q(v.state) << ",\"owned\":" << (v.item.owned ? "true" : "false")
            << ",\"occurrence\":" << v.item.occurrence << ",\"current_owned_item\":" << q(v.currentUri)
            << ",\"last_fault\":" << q(v.lastFault) << ",\"owned_item_uris\":[";
        for (size_t i = 0; i < v.ownedUris.size(); ++i) {
            if (i)
                out << ',';
            out << q(v.ownedUris[i]);
        }
        out << "]}";
        return out.str();
    }
    std::ostringstream o;
    o << "{\"requested\":" << q(v.requested) << ",\"active\":" << q(v.active) << ",\"state\":" << q(v.state)
      << ",\"reason\":" << q(v.reason) << ",\"occurrence\":" << v.item.occurrence
      << ",\"epoch\":" << v.item.epoch << ",\"generation\":" << v.generation
      << ",\"owned\":" << (v.item.owned ? "true" : "false") << ",\"mirrored_ahead\":" << v.item.ahead
      << ",\"last_fallback\":" << q(v.lastFallback) << ",\"handoff_gap_ms\":" << v.gapMs
      << ",\"latched\":" << (v.latched ? "true" : "false");
    if (v.requested == "queue")
        o << ",\"lms_cli_slow_replies\":" << v.lmsSlow << ",\"lms_cli_failed_replies\":" << v.lmsFailed;
    o << "}";
    return o.str();
}
inline std::atomic<RoomMode *> &modeRegistry() {
    static auto *p = new std::atomic<RoomMode *>(nullptr);
    return *p;
}
inline RoomMode *roomMode() { return modeRegistry().load(); }
inline void setRoomMode(RoomMode *p) {
    modeRegistry() = p;
} // process-lifetime room registry; configured before Player::run
} // namespace bridge
