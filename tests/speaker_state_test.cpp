#include "upnp/event_parsers.h"
#include "upnp/monitor.h"
#include "upnp/targets.h"
#include "upnp/subscriptions.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace upnp;
static std::string file(const char* name) { std::ifstream f(name); assert(f); return {std::istreambuf_iterator<char>(f), {}}; }
int main() {
    StateUpdate u;
    assert(parseAvTransport(file("tests/fixtures/sonos-lastchange.xml"), u) && *u.state == "PLAYING");
    assert(parseRenderingControl(file("tests/fixtures/rendering-lastchange.xml"), u) && *u.volume == 15);
    std::vector<Speaker> rooms;
    assert(parseZoneGroupEvent(file("tests/fixtures/zone-group-notify.xml"), rooms) && rooms.size() == 3);
    for (const auto bad : {"<broken>", "<propertyset><property><LastChange>&lt;Event&gt;</LastChange></property></propertyset>"}) {
        assert(!parseAvTransport(bad, u)); assert(!parseRenderingControl(bad, u)); assert(!parseZoneGroupEvent(bad, rooms));
    }
    auto wrap = [](const std::string& text) { return "<propertyset><property><LastChange>" + xmlEscape(text) + "</LastChange></property></propertyset>"; };
    assert(!parseAvTransport(wrap("<Event><InstanceID val='1'><TransportState val='PLAYING'/></InstanceID></Event>"), u));
    assert(parseAvTransport(wrap("<Event><InstanceID val='0'><CurrentTrackDuration val='0:03:00'/><CurrentTrackMetaData val='&lt;DIDL-Lite&gt;&lt;item&gt;&lt;dc:title&gt;A &amp;amp; B&lt;/dc:title&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;'/></InstanceID></Event>"), u));
    assert(!u.state && *u.title == "A & B" && *u.duration == "0:03:00");
    assert(parseAvTransport(wrap("<Event><InstanceID val='0'><AVTransportURI val='radio'/><CurrentTrackURI val='track'/><TransportStatus val='OK'/></InstanceID></Event>"), u));
    assert(*u.uri == "radio" && *u.trackUri == "track" && *u.status == "OK");
    assert(parseAvTransport(wrap("<Event><InstanceID val='0'/></Event>"), u) && !u.state && !u.uri);
    assert(!parseRenderingControl(wrap("<Event><InstanceID val='0'><Volume channel='Master' val='101'/></InstanceID></Event>"), u));
    std::cout << "PASS: AVTransport, RenderingControl and topology golden parsers; malformed, absent, metadata-only and wrong-instance deltas\n";
    SpeakerState state; const auto t = StateClock::time_point{};
    StateUpdate start; start.state = "STOPPED"; state.apply(start, t);
    StateUpdate poll; poll.startedRevision = state.revision; poll.state = "STOPPED"; poll.volume = 12;
    StateUpdate event; event.source = Source::Event; event.state = "TRANSITIONING"; state.apply(event, t);
    state.apply(poll, t);
    assert(state.transport.state == "TRANSITIONING" && state.volume == 12);
    poll.startedRevision = state.revision; state.apply(poll, t); assert(state.paused());
    StateUpdate oldMetadata; oldMetadata.startedRevision = state.revision;
    oldMetadata.uri = "old"; oldMetadata.title = "old"; oldMetadata.duration = "old"; oldMetadata.volume = 1;
    StateUpdate metadataEvent; metadataEvent.source = Source::Event;
    metadataEvent.uri = ""; metadataEvent.title = "new"; metadataEvent.duration = "0:02:00"; metadataEvent.volume = 42;
    state.apply(metadataEvent, t); state.apply(oldMetadata, t);
    assert(state.transport.uriKnown && state.transport.uri.empty() && state.transport.title == "new"
        && state.transport.duration == "0:02:00" && state.volume == 42);
    std::cout << "PASS: per-field events beat older polls; unrelated fields and polls started after events apply\n";
    SubscriptionSchedule schedule;
    schedule.success(t, 300); assert(schedule.due == t + std::chrono::seconds(150));
    schedule.failure(t); assert(schedule.due == t + timing::retryFirst);
    schedule.failure(t); assert(schedule.due == t + timing::retryLater);
    schedule.success(t, 2); schedule.failure(t); assert(schedule.due == t + timing::retryFirst);
    std::cout << "PASS: fake-clock subscription TTL half-life, one-second first retry and five-second subsequent retries\n";
    MonitorPolicy policy; std::array<bool,3> active{{true,true,true}};
    for (int ms = 0; ms < 30000; ms += 500) {
        const auto w = policy.next(t + std::chrono::milliseconds(ms), state, active);
        assert(!w.transport && !w.media && !w.volume && !w.topology && !w.position);
    }
    assert(policy.next(t + timing::sanity, state, active).transport);
    active[1] = false;
    auto w = policy.next(t + timing::sanity, state, active); assert(w.volume && !w.transport && !w.media && !w.topology);
    active[1] = true; assert(!policy.next(t + timing::sanity + timing::position, state, active).volume);
    active[2] = false; w = policy.next(t + timing::sanity + timing::position, state, active); assert(w.topology && !w.volume);
    for (size_t lost = 0; lost < 3; ++lost) {
        MonitorPolicy fallback; const std::array<bool,3> healthy{{true,true,true}};
        fallback.next(t, state, healthy);
        auto missing = healthy; missing[lost] = false;
        const auto isolated = fallback.next(t + timing::position, state, missing);
        assert(isolated.transport == (lost == 0));
        assert(isolated.volume == (lost == 1));
        assert(isolated.topology == (lost == 2));
        const auto restored = fallback.next(t + timing::position, state, healthy);
        assert(!restored.transport && !restored.media && !restored.volume && !restored.topology);
    }
    SpeakerState playing; StateUpdate play; play.state = "PLAYING"; playing.apply(play, t);
    MonitorPolicy activePosition;
    assert(activePosition.next(t, playing, {{true,true,true}}).position);
    assert(!activePosition.next(t + std::chrono::milliseconds(500), playing, {{true,true,true}}).position);
    assert(activePosition.next(t + timing::position, playing, {{true,true,true}}).position);
    MonitorPolicy legacy; legacy.legacy = true; legacy.stoppedMediaInfo = true;
    w = legacy.next(t, state, {{true,true,true}}); assert(w.transport && w.media && w.volume && w.topology && w.position);
    w = legacy.next(t + std::chrono::milliseconds(500), state, {{true,true,true}}); assert(w.transport && !w.media && !w.volume && !w.topology && !w.position);
    std::cout << "PASS: fake-clock monitor events sanity, service-specific fallback/recovery and legacy intervals\n";
    assert(topologyUpdate(rooms, "Sonos Port", "", u)); state.apply(u, t);
    assert(targetFor("Play", state) == "127.0.0.1" && targetFor("GetVolume", state) == "127.0.0.2");
    assert(targetFor("GetZoneGroupState", state) == "127.0.0.2");
    u.group->ip = "127.0.0.3"; state.apply(u, t); assert(targetFor("Stop", state) == "127.0.0.3");
    assert(topologyUpdate(rooms, "Study", "", u)); state.apply(u, t); assert(targetFor("Play", state) == state.room.ip);
    u.group->members = {"Study"}; state.apply(u, t); assert(targetFor("GetMediaInfo", state) == state.room.ip);
    std::cout << "PASS: action targets for solo, coordinator, group member and coordinator change\n";
}
