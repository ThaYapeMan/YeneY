// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "stream_pacing.h"
#include "start_lead.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <unistd.h>
namespace bridge {
inline uint64_t streamMonoMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct StreamMeasurement { double encodedMs = 0, audibleMs = 0; bool valid = false; };
// One room per process. Totals survive reconnects and stream changes; a pid
// identifies restarts. No measurement influences admission or transport.
class StreamMetrics {
    std::mutex mutex;
    std::string roomHex, path;
    std::function<StreamMeasurement()> provider;
    uint64_t lastFile = 0, lastLog = 0;
public:
    std::atomic<uint64_t> overwrites{0}, dropped{0}, socketBlockedUs{0}, socketBlockedCalls{0};
    static std::string hex(const std::string &s) {
        std::string out;
        for (unsigned char c : s) { char b[3]; snprintf(b, sizeof b, "%02x", c); out += b; }
        return out;
    }
    void configure(const std::string &room, const std::string &directory = "/var/lib/yeney") {
        std::lock_guard<std::mutex> lock(mutex);
        roomHex = hex(room);
        path = directory + "/stream-pacing-" + roomHex + ".json";
    }
    void encoder(std::function<StreamMeasurement()> p) {
        std::lock_guard<std::mutex> lock(mutex); provider = std::move(p);
    }
    StreamMeasurement sample() {
        std::function<StreamMeasurement()> p;
        { std::lock_guard<std::mutex> lock(mutex); p = provider; }
        return p ? p() : StreamMeasurement{};
    }
    void socketWait(uint64_t us) {
        // A blocking send's residence time, excluding sub-millisecond syscall
        // overhead. Includes a timed-out send; does not change socket behavior.
        if (us >= 1000) { socketBlockedUs += us; ++socketBlockedCalls; }
    }
    void print(const StreamMeasurement &s) {
        printf("Stream pacing: mode=%s start_lead_ms=%u overwrites=%llu dropped=%llu socket_backpressure_ms=%.3f encoded_ms=%.3f audible_ms=%.3f lead_ms=%.3f valid=%d\n",
            streamPacingSonos() ? "sonos" : "wall", yeney_start_lead_ms(),
            (unsigned long long)overwrites.load(), (unsigned long long)dropped.load(),
            socketBlockedUs.load()/1000., s.encodedMs, s.audibleMs, s.encodedMs-s.audibleMs, s.valid);
    }
    bool tick(uint64_t now = streamMonoMs()) {
        { std::lock_guard<std::mutex> lock(mutex);
          if (roomHex.empty() || (now-lastFile < 1000 && now-lastLog < 60000)) return false; }
        auto s = sample();
        std::lock_guard<std::mutex> lock(mutex);
        if (roomHex.empty()) return false;
        if (now-lastFile >= 1000) {
            lastFile = now;
            // Rename a complete snapshot; readers never see a partial JSON file.
            auto temporary = path + "." + std::to_string(getpid()) + ".tmp";
            if (FILE *f = fopen(temporary.c_str(), "w")) {
                fprintf(f, "{\"mode\":\"%s\",\"start_lead_ms\":%u,\"pid\":%d,\"monotonic_ms\":%llu,\"overwrites\":%llu,\"dropped\":%llu,\"socket_backpressure_ms\":%.3f,\"socket_backpressure_calls\":%llu,\"encoded_ms\":%.3f,\"audible_ms\":%.3f,\"lead_ms\":%.3f,\"valid\":%s}\n",
                    streamPacingSonos() ? "sonos" : "wall", yeney_start_lead_ms(), getpid(), (unsigned long long)now, (unsigned long long)overwrites.load(),
                    (unsigned long long)dropped.load(), socketBlockedUs.load()/1000.,
                    (unsigned long long)socketBlockedCalls.load(), s.encodedMs, s.audibleMs,
                    s.encodedMs-s.audibleMs, s.valid ? "true" : "false");
                if (fclose(f) == 0) rename(temporary.c_str(), path.c_str());
                unlink(temporary.c_str());
            }
        }
        if (now-lastLog < 60000 || (!overwrites && !dropped && !socketBlockedUs && (!s.valid || s.encodedMs == s.audibleMs))) return false;
        lastLog = now;
        printf("yeney: stream-pacing room_hex=%s reason=%s ", roomHex.c_str(), dropped ? "reader-stalled" : "sample"); print(s);
        return true;
    }
};
inline StreamMetrics &streamMetrics() { static StreamMetrics metrics; return metrics; }
}
