// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
namespace bridge {
struct ModeStatus {
    std::string requested = "stream", active = "stream", state = "active", reason = "default-stream";
    bool owned = false;
    uint64_t occurrence = 0;
    std::vector<std::string> ownedUris;
    std::string currentUri, lastFault;
};
// Passive snapshot registry, never a mode controller. A single room worker
// publishes queue observations and the complete diagnostic file synchronously.
class RoomStatus {
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
    static std::string json(const ModeStatus &v) {
        auto q = [](const std::string &t) {
            std::string out = "\"";
            for (unsigned char c : t) {
                if (c == '"' || c == '\\')
                    out += '\\';
                out += c >= 32 ? char(c) : '?';
            }
            return out + '"';
        };
        std::ostringstream out;
        out << "{\"requested\":" << q(v.requested) << ",\"active\":" << q(v.active)
            << ",\"state\":" << q(v.state) << ",\"reason\":" << q(v.reason);
        if (v.active == "queue") {
            out << ",\"occurrence\":" << v.occurrence << ",\"owned\":" << (v.owned ? "true" : "false")
                << ",\"current_owned_item\":" << q(v.currentUri) << ",\"last_fault\":" << q(v.lastFault)
                << ",\"owned_item_uris\":[";
            for (size_t i = 0; i < v.ownedUris.size(); ++i) {
                if (i)
                    out << ',';
                out << q(v.ownedUris[i]);
            }
            out << "]}";
        } else {
            // Preserve the stream diagnostic's established representation.
            out << ",\"occurrence\":0,\"epoch\":0,\"generation\":1,\"owned\":false,\"mirrored_ahead\":0,"
                   "\"last_fallback\":\"\",\"handoff_gap_ms\":-1,\"latched\":false";
            if (v.requested == "queue")
                out << ",\"lms_cli_slow_replies\":0,\"lms_cli_failed_replies\":0";
            out << '}';
        }
        return out.str();
    }
};
inline std::atomic<RoomStatus *> &statusRegistry() {
    static auto *registry = new std::atomic<RoomStatus *>(nullptr);
    return *registry;
}
inline RoomStatus *roomMode() { return statusRegistry().load(); }
inline void setRoomMode(RoomStatus *value) { statusRegistry() = value; }
} // namespace bridge
