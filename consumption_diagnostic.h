// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "upnp/stream_server.h"
#include "player_name.h"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace bridge {
inline uint64_t consumptionTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// One native player/room per process. This is the core TIMING-01 callback,
// independent of SHM publication settings. It describes the next offered block,
// not a claim that this sample is already in the speaker's HTTP buffer.
struct ContentObservation {
    std::mutex mutex;
    uint64_t frame = 0, at = 0;
    bool known = false;
    void set(uint64_t value, bool valid) {
        std::lock_guard<std::mutex> lock(mutex);
        frame = value; known = valid; at = consumptionTimeMs();
    }
};
inline ContentObservation &consumptionContent() { static ContentObservation value; return value; }
inline std::string consumptionLine(uint64_t now, uint64_t served, int64_t backlog,
                                   uint64_t frame, bool known, uint64_t observed,
                                   const std::string &roomHex, const char *mode) {
    const auto bytes = served == UINT64_MAX ? "unknown" : std::to_string(served);
    const auto pending = backlog < 0 ? "unknown" : std::to_string(backlog);
    const auto audible = backlog < 0 || served == UINT64_MAX ? "unknown" :
        std::to_string(served > uint64_t(backlog) ? served - uint64_t(backlog) : 0);
    return "yeney: queue consume room_hex=" + roomHex + " mode=" + mode +
        " native_ms=" + std::to_string(now) + " served=" + bytes + " backlog=" + pending +
        " audible=" + audible + " content_frame=" + (known ? std::to_string(frame) : "unknown") +
        " content_observed_ms=" + std::to_string(observed) + "\n";
}
// Separate observer: stalled send/read calls cannot suppress the 1 Hz samples.
// No socket options, writes, transport calls or audio counters are changed.
class ConsumptionDiagnostic {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
    std::thread worker;
    inline static std::atomic<uint64_t> serial{0};
public:
    ConsumptionDiagnostic(upnp::StreamRequest &request, const std::string &room, const char *mode) {
        std::string hex;
        constexpr char digits[] = "0123456789abcdef";
        for (unsigned char c : room) { hex += digits[c >> 4]; hex += digits[c & 15]; }
        const auto connection = ++serial;
        worker = std::thread([this, &request, hex, mode, connection] {
            std::unique_lock<std::mutex> lock(mutex);
            auto next = std::chrono::steady_clock::now();
            while (!stopped) {
                const auto now = consumptionTimeMs();
                const auto served = request.connectionSentBytes();
                const auto backlog = request.connectionBacklog();
                auto &content = consumptionContent();
                std::string line;
                { std::lock_guard<std::mutex> contentLock(content.mutex);
                  line = consumptionLine(now, served, backlog, content.frame, content.known, content.at, hex, mode); }
                line.insert(line.size()-1, " connection=" + std::to_string(connection));
                std::fputs(line.c_str(), stdout);
                next += std::chrono::seconds(1);
                changed.wait_until(lock, next, [this] { return stopped; });
            }
        });
    }
    ~ConsumptionDiagnostic() {
        { std::lock_guard<std::mutex> lock(mutex); stopped = true; }
        changed.notify_one(); worker.join();
    }
};
} // namespace bridge
