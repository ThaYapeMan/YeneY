// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "playback_mode.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
namespace bridge {
struct ModeEvidence {
    uint64_t generation = 0, epoch = 0, occurrence = 0, token = 0, frame = 0, promised = 0, actual = 0;
    unsigned rate = 0, index = 0, sourceMs = 0, ahead = 0;
    bool owned = false, playing = false, ended = false, usable = true;
};
struct ModeStatus {
    std::string requested = "stream", active = "stream", state = "active", reason = "default-stream",
                lastFallback;
    ModeEvidence item;
    uint64_t generation = 1;
    unsigned oldRate = 0, newRate = 0;
    bool latched = false;
    uint64_t lmsSlow = 0, lmsFailed = 0;
    double gapMs = -1;
};
class RoomMode {
public:
    struct Hooks {
        // All hooks execute on the room worker, outside the status lock.
        std::function<void(const ModeEvidence &)> stopOwned, release;
        std::function<void(uint64_t, const ModeEvidence &)> stream;
        std::function<void(const std::string &)> log;
    };

private:
    mutable std::mutex mutex;
    ModeStatus s;
    Hooks hooks;
    std::string room, coordinator;
    bool stopped = false, freezeFailed = false, started = false, rearmBlocked = false;
    std::deque<uint64_t> rearms;
    std::deque<char> commands;
    std::function<void()> loopHandler;
    uint64_t endAt = 0, prepareAt = 0;
    void log(const ModeStatus &v) const;

public:
    RoomMode(std::string room, std::string coordinator, PlaybackSelection, Hooks);
    ModeStatus status() const;
    bool accepts(uint64_t generation, bool queue) const;
    // Call on the serialized room worker; now is monotonic milliseconds.
    bool newPlay(bool ready, uint64_t epoch, uint64_t now);
    void beginPreparation(uint64_t now);
    void deferPreparation(uint64_t ms) {
        std::lock_guard<std::mutex> l(mutex);
        if (s.state == "prepare" && prepareAt)
            prepareAt += ms;
    }
    void lmsMetrics(uint64_t slow, uint64_t failed) {
        std::lock_guard<std::mutex> l(mutex);
        s.lmsSlow = std::max(s.lmsSlow, slow);
        s.lmsFailed = std::max(s.lmsFailed, failed);
    }
    void onRearmLoop(std::function<void()> handler) { loopHandler = std::move(handler); }
    void command(char); // serialized command history for loop diagnostics
    void stop();        // explicit LMS q only, not seek/refetch f
    void evidence(ModeEvidence, uint64_t now);
    void request(const std::string &reason, uint64_t now);
    void tick(uint64_t now);
    void streamAudible(uint64_t generation, uint64_t now, unsigned rate);
    static std::string json(const ModeStatus &);
};
inline ModeStatus RoomMode::status() const {
    std::lock_guard<std::mutex> l(mutex);
    return s;
}
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
