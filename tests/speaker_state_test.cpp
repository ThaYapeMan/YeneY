#include "upnp/event_parsers.h"
#include "../queue_speaker_changes.h"
#include "upnp/monitor.h"
#include "upnp/targets.h"
#include "upnp/subscriptions.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace upnp;
static std::string file(const char* name) { std::ifstream f(name); assert(f); return {std::istreambuf_iterator<char>(f), {}}; }
int main() {
    const std::string queueBody = "<item id=\"yeney:secret-session\">&amp;quot;yeney:second-session&amp;quot;<CurrentTrackURI>http://bridge/yeney/q/secret-session/2/1/lazy-7.wav</CurrentTrackURI>"
        "&lt;res&gt;http://bridge/yeney/q/second-session/2/1/8.wav&lt;/res&gt;";
    const auto logged = logValue(queueBody);
    assert(logged.find("secret-session") == std::string::npos && logged.find("second-session") == std::string::npos);
    assert(logged == queueUriLog(queueBody));
    const auto cut = queueBody.find("secret-session") + 4;
    assert(logValue(queueBody, cut) == queueUriLog(queueBody).substr(0, cut));
    assert(logValue("ordinary\nvalue") == "ordinary value");
    std::cout << "PASS queue NOTIFY logging: URI sessions masked in raw and escaped XML before truncation; ordinary diagnostics unchanged\n";

    {
        bridge::SpeakerChanges changes;
        auto event = [](unsigned sequence, const std::string &uri, const std::string &state) {
            GenaEvent e;
            e.sid = "test"; e.sequence = sequence;
            e.body = "<propertyset><property><LastChange>" + xmlEscape(
                "<Event><InstanceID val='0'><CurrentTrack val='2'/><CurrentTrackURI val='"+uri+
                "'/><NumberOfTracks val='5'/><AVTransportURI val='queue'/><TransportState val='"+state+
                "'/></InstanceID></Event>") + "</LastChange></property></propertyset>";
            return e;
        };
        changes.expectEdit("old", 2, 5, "queue");
        const auto started = std::chrono::steady_clock::now();
        assert(!changes.event(event(1, "old", "TRANSITIONING")));
        assert(!changes.event(event(2, "old", "PLAYING")));
        assert(changes.poll(started, {{"CurrentTrackURI", "new"}}).at("CurrentTrackURI") == "new");
        assert(changes.event(event(3, "next", "PLAYING"))); // App Next is real evidence.
        assert(changes.poll(started, {{"CurrentTrackURI", "old"}}).at("CurrentTrackURI") == "next");
        changes.expectEdit("next", 2, 5, "queue");
        assert(changes.event(event(4, "next", "PAUSED_PLAYBACK"))); // Do not fence app pause.
        assert(!changes.event(event(3, "old", "PLAYING"))); // Subscription ordering remains enforced.
        auto partial = event(5, "old", "PLAYING");
        partial.body = "<propertyset><property><LastChange>" + xmlEscape(
            "<Event><InstanceID val='0'><CurrentTrack val='2'/><NumberOfTracks val='5'/></InstanceID></Event>") +
            "</LastChange></property></propertyset>";
        assert(!changes.event(partial));
        assert(changes.poll(std::chrono::steady_clock::now(), {{"CurrentTrackURI", "next"}}).at("CurrentTrackURI") == "next");
        std::cout << "PASS self-edit GENA fence: delayed old URI cannot supersede target poll; app Next/pause and SEQ retained\n";
    }

    StateUpdate u;
    assert(parseAvTransport(file("tests/fixtures/sonos-lastchange.xml"), u) && *u.state == "PLAYING");
    assert(parseRenderingControl(file("tests/fixtures/rendering-lastchange.xml"), u) && *u.volume == 26);
    std::vector<Speaker> rooms;
    assert(parseZoneGroupEvent(file("tests/fixtures/zone-group-notify.xml"), rooms) && rooms.size() == 3);
    for (const auto& room : rooms) {
        assert(room.coordinator == room.name && room.members == std::vector<std::string>{room.name});
        if (room.name == "Study") {
            assert(room.uuid == "RINCON_949F3EFABA6601400" && room.ip == "192.168.178.132");
            assert(room.firmware == "86.10-80260");
        } else if (room.name == "MBR") {
            assert(room.ip == "192.168.178.145" && room.firmware == "86.10-80260");
        } else {
            assert(room.name == "Sonos Port" && room.ip == "192.168.178.140" && room.firmware == "97.1-80312");
        }
    }
    std::cout << "PASS: captured Master volume 26 ignores LF/RF 100; Study/MBR/Port solo groups, real IPs, UUID and firmware\n";
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
    assert(parseAvTransport(wrap("<Event><InstanceID val='0'><CurrentTrack val='3'/><CurrentTrackURI val='owned-3'/><RelativeTimePosition val='0:01:02'/></InstanceID></Event>"), u, true));
    assert(u.trackNumber == 3 && u.trackUri == "owned-3" && u.positionMs == 62000);
    SpeakerState queueState; queueState.apply(u, StateClock::now());
    StateUpdate stale; stale.startedRevision = 0; stale.trackNumber = 2; stale.trackUri = "owned-2"; stale.positionMs = 1000;
    queueState.apply(stale, StateClock::now());
    assert(queueState.trackNumberKnown && queueState.trackNumber == 3 && queueState.trackUri == "owned-3" && queueState.positionMs == 62000);
    for (const auto bad : {"-1", "4294967296", "3x", ""})
        assert(!parseAvTransport(wrap("<Event><InstanceID val='0'><CurrentTrack val='"+std::string(bad)+"'/></InstanceID></Event>"), u, true));
    assert(!parseAvTransport(wrap("<Event><InstanceID val='0'><RelativeTimePosition val='0:99:00'/></InstanceID></Event>"), u, true));
    std::cout << "PASS queue GENA identity: TrackNumber/URI/RelTime retained together, stale polls fenced, invalid ordinals rejected\n";
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
    // Keep the synthetic grouped/coordinator-change coverage independently of
    // the real capture, whose three rooms are all solo.
    rooms = parseTopology(file("tests/fixtures/topology.xml"));
    assert(topologyUpdate(rooms, "Sonos Port", "", u)); state.apply(u, t);
    assert(targetFor("Play", state) == "127.0.0.1" && targetFor("GetVolume", state) == "127.0.0.2");
    assert(targetFor("GetZoneGroupState", state) == "127.0.0.2");
    u.group->ip = "127.0.0.3"; state.apply(u, t); assert(targetFor("Stop", state) == "127.0.0.3");
    assert(topologyUpdate(rooms, "Study", "", u)); state.apply(u, t); assert(targetFor("Play", state) == state.room.ip);
    u.group->members = {"Study"}; state.apply(u, t); assert(targetFor("GetMediaInfo", state) == state.room.ip);
    std::cout << "PASS: action targets for solo, coordinator, group member and coordinator change\n";
}
