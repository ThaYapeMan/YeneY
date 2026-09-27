#pragma once
#include "speaker_control.h"
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
namespace upnp {
using StateClock = std::chrono::steady_clock;
enum class Source { Event, Poll };
enum class Service { AVTransport, RenderingControl, ZoneGroupTopology };
constexpr size_t serviceIndex(Service service) { return static_cast<size_t>(service); }
inline const char* serviceName(Service s) {
    return s == Service::AVTransport ? "AVTransport" : s == Service::RenderingControl ? "RenderingControl" : "ZoneGroupTopology";
}
inline const char* eventPath(Service s) {
    return s == Service::AVTransport ? "/avt" : s == Service::RenderingControl ? "/rc" : "/zgt";
}
inline const char* subscriptionPath(Service s) {
    return s == Service::AVTransport ? "/MediaRenderer/AVTransport/Event" :
        s == Service::RenderingControl ? "/MediaRenderer/RenderingControl/Event" : "/ZoneGroupTopology/Event";
}
struct Group { std::string name, uuid, ip; std::vector<std::string> members; };
struct StateUpdate {
    Source source = Source::Poll;
    uint64_t startedRevision = 0;
    std::optional<std::string> state, status, uri, trackUri, title, duration;
    std::optional<bool> available;
    std::optional<uint8_t> volume;
    std::optional<Speaker> room;
    std::optional<Group> group;
    std::optional<uint32_t> positionMs;
    std::optional<std::string> positionText;
};
struct SpeakerState {
    TransportInfo transport;
    std::string trackUri;
    uint8_t volume = 0;
    Speaker room;
    Group group;
    uint32_t positionMs = 0;
    std::string positionText;
    bool positionKnown = false;
    Source source = Source::Poll;
    uint64_t revision = 0;
    StateClock::time_point updatedAt{};
    bool paused() const { return transport.state == "STOPPED" || transport.state == "PAUSED_PLAYBACK"; }
    bool playing() const { return transport.state == "PLAYING" || transport.state == "TRANSITIONING"; }
    bool apply(const StateUpdate& u, StateClock::time_point now) {
        bool changed = false;
        size_t field = 0;
        const auto next = revision + 1;
        auto put = [&](auto& value, const auto& delta) {
            const auto slot = field++;
            if (!delta || (u.source == Source::Poll && events[slot] > u.startedRevision)) return false;
            value = *delta;
            if (u.source == Source::Event) events[slot] = next;
            changed = true;
            return true;
        };
        put(transport.state, u.state); put(transport.status, u.status);
        if (put(transport.uri, u.uri)) transport.uriKnown = true;
        put(trackUri, u.trackUri); put(transport.title, u.title); put(transport.duration, u.duration);
        put(transport.available, u.available); put(volume, u.volume);
        put(room, u.room); put(group, u.group);
        if (put(positionMs, u.positionMs)) positionKnown = true;
        put(positionText, u.positionText);
        if (changed) { revision = next; source = u.source; updatedAt = now; }
        return changed;
    }
private:
    std::array<uint64_t, 12> events{};
};
// All readers receive a value copy; no state lock is held across I/O.
class SpeakerStateStore {
    mutable std::mutex mutex;
    SpeakerState value;
public:
    SpeakerState snapshot() const { std::lock_guard<std::mutex> lock(mutex); return value; }
    void apply(const StateUpdate& u, StateClock::time_point now = StateClock::now()) {
        std::lock_guard<std::mutex> lock(mutex); value.apply(u, now);
    }
};
}
