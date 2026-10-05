#include "own_speaker_control.h"
#include "discovery.h"
#include "http.h"
#include "../timing_probe_runtime.h"
#include <cstdio>
#include <limits>
#include <cstdlib>
namespace upnp {
OwnSpeakerControl::OwnSpeakerControl(std::function<unsigned()> port, unsigned controlPort,
                                     std::function<StreamActivity()> activity, std::function<void()> callback,
                                     std::shared_ptr<HttpServer> server)
    : streamPort(std::move(port)), speakerPort(controlPort), streamActivity(std::move(activity)),
      eventCallback(std::move(callback)), eventServer(std::move(server)) {
    (void)streamContentMode();
    const char* mode = std::getenv("YENEY_POLL");
    monitor.legacy = mode && std::string(mode) == "legacy";
    if (mode && std::string(mode) != "events" && std::string(mode) != "legacy")
        printf("yeney: setting key=YENEY_POLL invalid=%s fallback=events\n", logValue(mode).c_str());
    const char* media = std::getenv("YENEY_STOPPED_MEDIAINFO");
    monitor.stoppedMediaInfo = media && std::string(media) == "1";
    if (media && std::string(media) != "0" && std::string(media) != "1")
        printf("yeney: setting key=YENEY_STOPPED_MEDIAINFO invalid=%s fallback=0\n", logValue(media).c_str());
    if (timing_probe::enabled()) {
        (void)timing_probe::diagnostics(); // Construct before worker/atexit cleanup.
        timingThread = std::thread(&OwnSpeakerControl::timingLoop, this);
    }
    printf("yeney: settings poll=%s stopped_mediainfo=%d\n", monitor.legacy ? "legacy" : "events", monitor.stoppedMediaInfo);
}
OwnSpeakerControl::~OwnSpeakerControl() { shutdownEvents(); }
void OwnSpeakerControl::shutdownEvents() {
    const bool first=!timingStop.exchange(true);
    if (timingThread.joinable()) timingThread.join();
    if (first && timing_probe::enabled()) timing_probe::diagnostics().reset("shutdown",true);
    subscriptions.reset();
}
void OwnSpeakerControl::startEvents() {
    try {
        if (!eventServer) eventServer = std::make_shared<HttpServer>(0);
        subscriptions.reset(new Subscriptions(speakerPort, *eventServer, [this] {
            const auto s = state.snapshot();
            return std::array<std::string, 3>{{s.group.ip, s.room.ip, s.room.ip}};
        }, [this](const GenaEvent& event) { return receiveEvent(event); }));
    } catch (const std::exception& error) {
        printf("yeney: subscription result=unavailable reason=%s\n", logValue(error.what()).c_str());
    }
}
void OwnSpeakerControl::apply(StateUpdate update) {
    const auto before = state.snapshot();
    if (update.title) {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        const auto baseStart = sentUrl.rfind('/', sentUrl.find('?'));
        const auto basename = sentUrl.substr(baseStart == std::string::npos ? 0 : baseStart + 1);
        const auto bare = basename.substr(0, basename.find('?'));
        if (update.title->empty() || *update.title == sentUrl || *update.title == sentUri
            || *update.title == basename || *update.title == bare) update.title = sentTitle;
    }
    state.apply(update);
    const auto after = state.snapshot();
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        if (before.paused() && !after.paused()) freshStreamPosition = true;
        if (!after.paused() || !before.paused()) pauseTimeoutLogged = false;
    }
    if (update.group && (before.group.uuid != after.group.uuid || before.group.members != after.group.members))
        printf("yeney: group room=%s coordinator=%s members=%s\n", logValue(after.room.name).c_str(),
            logValue(after.group.name).c_str(), logValue(groupDescription(after.room)).c_str());
}
bool OwnSpeakerControl::receiveEvent(const GenaEvent& event) {
    auto update = event.update;
    update.source = Source::Event;
    if (event.service == Service::ZoneGroupTopology) {
        const auto previous = state.snapshot();
        if (!topologyUpdate(event.topology, room, previous.room.uuid, update)) return false;
    }
    apply(update);
    static const bool debugEvents = [] {
        const char* value = std::getenv("YENEY_DEBUG_EVENTS"); return value && std::string(value) == "1";
    }();
    if (debugEvents) printf("yeney: event service=%s TransportState=%s seq=%u\n",
        serviceName(event.service), logValue(state.snapshot().transport.state).c_str(), event.sequence);
    if (eventCallback) eventCallback();
    return true;
}
Speaker OwnSpeakerControl::speaker() const { return state.snapshot().room; }
TransportInfo OwnSpeakerControl::transportInfo() { return state.snapshot().transport; }
uint8_t OwnSpeakerControl::displayVolume() { return state.snapshot().volume; }
std::string OwnSpeakerControl::controllerUri() {
    const auto port = streamPort();
    std::lock_guard<std::mutex> lock(runtimeMutex);
    return localAddress.empty() || !port ? "" : "http://" + localAddress + ":" + std::to_string(port);
}
unsigned OwnSpeakerControl::actionTimeoutMs(const std::string& action) {
    return action == "Play" || action == "SetAVTransportURI" || action == "Stop" || action == "Pause"
        ? timing::transportMs : timing::readMs;
}
SoapResult OwnSpeakerControl::call(const std::string& action, const SoapArguments& args,
                                  const std::string& host, const std::string& service) {
    const std::string address = host.empty() ? targetFor(action, state.snapshot()) : host;
    const auto path = service == "ZoneGroupTopology" ? "/ZoneGroupTopology/Control" :
        service == "RenderingControl" ? "/MediaRenderer/RenderingControl/Control" : "/MediaRenderer/AVTransport/Control";
    const auto activityBefore = action == "GetPositionInfo" && streamActivity ? streamActivity() : StreamActivity{};
    auto http = httpPost({address, path, speakerPort}, {
        {"Content-Type", "text/xml"}, {"SOAPACTION", "\"urn:schemas-upnp-org:service:" + service + ":1#" + action + "\""}
    }, soapBody(service, action, args), actionTimeoutMs(action));
    if (!http.localAddress.empty()) { std::lock_guard<std::mutex> lock(runtimeMutex); localAddress = http.localAddress; }
    auto result = parseSoap(http.body, action);
    if (!result.faultCode.empty() || !result.faultDescription.empty()) {
        printf("yeney: SOAP action=%s fault=%s description=%s\n", action.c_str(), logValue(result.faultCode).c_str(), logValue(result.faultDescription).c_str());
    } else if (!http.error.empty() || http.status != 200 || !result.ok) {
        const auto activity = streamActivity ? streamActivity() : StreamActivity{};
        const auto paused = state.snapshot().paused();
        std::lock_guard<std::mutex> lock(runtimeMutex);
        if (action == "GetPositionInfo" && http.error == "timeout" && paused && (activityBefore.requestOpen || activity.requestOpen)) {
            if (!pauseTimeoutLogged) printf("yeney: SOAP action=GetPositionInfo reason=no-reply-held-request state=paused\n");
            pauseTimeoutLogged = true;
        } else printf("yeney: SOAP action=%s error=%s HTTP=%u\n", action.c_str(),
                     http.error.empty() ? "invalid-response" : logValue(http.error).c_str(), http.status);
    }
    result.ok = result.ok && http.error.empty() && http.status == 200;
    return result;
}
bool OwnSpeakerControl::topology(const std::string& host, bool initial) {
    StateUpdate update; const auto previous = state.snapshot(); update.startedRevision = previous.revision;
    auto response = call("GetZoneGroupState", {}, host, "ZoneGroupTopology");
    if (!response.ok || !topologyUpdate(parseTopology(response.response.value("ZoneGroupState")), room,
        initial ? "" : previous.room.uuid, update)) return false;
    apply(update); return true;
}
bool OwnSpeakerControl::discover(const std::string& requestedRoom, const std::string& seed) {
    room = requestedRoom;
    std::vector<HttpUrl> locations;
    if (!seed.empty()) {
        HttpUrl url;
        if (!parseHttpUrl("http://" + seed + ":" + std::to_string(speakerPort), url)) return false;
        locations.push_back(url);
    } else locations = discoverSsdp();
    for (const auto& location : locations) {
        if (!topology(location.host, true)) continue;
        // Probe selected endpoint as well: seed may be reachable on another NIC.
        poll();
        if (!transportInfo().available || controllerUri().empty()) return false;
        if (!subscriptions) startEvents();
        return true;
    }
    printf("yeney: discovery room=%s result=failed\n", logValue(room).c_str());
    return false;
}
std::vector<std::string> OwnSpeakerControl::discoverRooms(const std::string& seed) {
    std::vector<HttpUrl> locations;
    if (!seed.empty()) {
        HttpUrl url;
        if (!parseHttpUrl("http://" + seed + ":" + std::to_string(speakerPort), url)) return {};
        locations.push_back(url);
    } else locations = discoverSsdp();
    std::vector<std::string> rooms;
    for (const auto& location : locations) {
        const auto result = call("GetZoneGroupState", {}, location.host, "ZoneGroupTopology");
        if (!result.ok) continue;
        for (const auto& speaker : parseTopology(result.response.value("ZoneGroupState")))
            rooms.push_back(speaker.name);
    }
    return rooms;
}
std::vector<Speaker> OwnSpeakerControl::discoverRoomDetails(const std::string& seed) {
    std::vector<HttpUrl> locations;
    if (seed.empty()) locations = discoverSsdp();
    else locations.push_back({seed, "/", speakerPort});
    for (const auto& location : locations) {
        auto result = call("GetZoneGroupState", {}, location.host, "ZoneGroupTopology");
        if (!result.ok) continue;
        auto rooms = parseTopology(result.response.value("ZoneGroupState"));
        if (rooms.empty()) continue;
        for (auto& room : rooms) room.model = deviceModel(room.location);
        return rooms;
    }
    return {};
}
bool OwnSpeakerControl::playStream(const std::string& url, const std::string& title, const std::string& art,
                                 const std::string& artist, const std::string& album) {
    if (url.find(':') == std::string::npos) return false;
    const auto metadata = streamDidl(url, title, art, artist, album);
    XmlNode item;
    if (!parseXml(metadata, item) || !item.child("item")) return false;
    const auto uri = item.child("item")->value("res");
    const auto displayTitle = item.child("item")->value("title");
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        freshStreamPosition = sentUrl != url;
        sentTitle = displayTitle; sentUri = uri; sentUrl = url;
    }
    StateUpdate update; update.startedRevision = state.snapshot().revision; update.title = displayTitle; apply(update);
    if (!call("SetAVTransportURI", {{"InstanceID", "0"}, {"CurrentURI", uri}, {"CurrentURIMetaData", metadata}}).ok)
        return false;
    // A successful URI assignment is an ownership observation too. Preserve
    // any newer URI event received during SOAP rather than overwriting it.
    update.title.reset(); update.uri = uri; apply(update);
    return play();
}
bool OwnSpeakerControl::play() { return call("Play", {{"InstanceID", "0"}, {"Speed", "1"}}).ok; }
bool OwnSpeakerControl::pause() { return call("Pause", {{"InstanceID", "0"}, {"Speed", "1"}}).ok; }
bool OwnSpeakerControl::stop() { return call("Stop", {{"InstanceID", "0"}, {"Speed", "1"}}).ok; }
bool OwnSpeakerControl::currentUri(std::string& uri) {
    StateUpdate update; update.startedRevision = state.snapshot().revision;
    const auto result = call("GetMediaInfo", {{"InstanceID", "0"}});
    const auto current = result.response.child("CurrentURI");
    if (!result.ok || !current) return false;
    update.uri = current->text; apply(update);
    uri = state.snapshot().transport.uri; return true;
}

void OwnSpeakerControl::timingLoop() {
    auto &diagnostic = timing_probe::diagnostics();
    while (!timingStop.load()) {
        const auto before = state.snapshot();
        const auto activity = streamActivity ? streamActivity() : StreamActivity{};
        std::string expected;
        {
            std::lock_guard<std::mutex> lock(runtimeMutex);
            expected = sentUri;
        }
        const auto anchor = diagnostic.snapshot();
        const std::string suffix = "&stream=" + std::to_string(anchor.stream);
        const bool currentStream =
            expected.size() >= suffix.size() &&
            expected.compare(expected.size() - suffix.size(), suffix.size(), suffix) == 0;
        const bool allowed = timing_probe::eligible(
            true, !before.group.uuid.empty() && before.room.uuid == before.group.uuid,
            before.transport.state == "PLAYING",
            before.transport.uriKnown && !expected.empty() && before.transport.uri == expected &&
                currentStream,
            activity.timingAllowed);
        diagnostic.speaker(before.room.uuid);
        const auto context = diagnostic.start(allowed, before.room.name);
        if (context.active && diagnostic.request(context, timing_probe::clockSeconds())) {
            ProbeHttpTiming stamp;
            probeHttpTiming = &stamp;
            const auto http = httpPost(
                {before.room.ip, "/MediaRenderer/AVTransport/Control", speakerPort},
                {{"Content-Type", "text/xml"},
                 {"SOAPACTION", "\"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo\""}},
                soapBody("AVTransport", "GetPositionInfo", {{"InstanceID", "0"}}), 200);
            probeHttpTiming = nullptr;
            const double completed = timing_probe::clockSeconds();
            const auto result = parseSoap(http.body, "GetPositionInfo");
            unsigned long long h = 0, m = 0, s = 0;
            char tail;
            const std::string relTime = result.response.value("RelTime");
            const bool parsed =
                sscanf(relTime.c_str(), "%llu:%llu:%llu%c", &h, &m, &s, &tail) == 3 && m < 60 &&
                s < 60 && h <= UINT32_MAX / 3600000;
            const auto after = state.snapshot();
            const auto current = streamActivity ? streamActivity() : StreamActivity{};
            const bool unchanged = after.room.uuid == before.room.uuid &&
                                   after.group.uuid == before.group.uuid &&
                                   after.transport.state == "PLAYING" &&
                                   after.transport.uri == expected && current.timingAllowed;
            if (!unchanged)
                diagnostic.start(false, before.room.name);
            const bool ok = http.error.empty() && http.status == 200 && result.ok && parsed &&
                            stamp.sent > 0 && stamp.received >= stamp.sent;
            const std::string reason = !http.error.empty()  ? http.error
                                       : http.status != 200 ? "http-status"
                                       : !result.ok         ? "soap-error"
                                       : !parsed            ? "invalid-reltime"
                                                            : "invalid-timestamp";
            diagnostic.response(context, ok, stamp.sent > 0 ? stamp.sent : NAN,
                                http.error.empty() && stamp.received > 0 ? stamp.received
                                                                         : completed,
                                parsed ? double(h * 3600 + m * 60 + s) : NAN, relTime,
                                http.error == "timeout" ? "timeout" : "error", reason);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool OwnSpeakerControl::positionInfo(uint32_t& ms, std::string* text) {
    const auto before = state.snapshot();
    const auto activity = streamActivity ? streamActivity() : StreamActivity{};
    auto cached = [&] { const auto s = state.snapshot(); ms = s.positionMs; if (text && s.positionKnown) *text = s.positionText; return s.positionKnown; };
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        const bool skip = !monitor.legacy ? !before.playing() : before.paused() && !activity.streaming && !freshStreamPosition;
        if (skip || positionReading || (Clock::now() < positionAt && !(monitor.legacy && freshStreamPosition))) return cached();
        positionReading = true; freshStreamPosition = false; positionAt = Clock::now() + timing::position;
    }
    const auto probeContext=timing_probe::enabled() ? timing_probe::diagnostics().snapshot() : timing_probe::Context{};
    const double probeStart=timing_probe::enabled() ? timing_probe::clockSeconds() : 0;
    const auto result = call("GetPositionInfo", {{"InstanceID", "0"}});
    { std::lock_guard<std::mutex> lock(runtimeMutex); positionReading = false; }
    if (!result.ok) {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        if (monitor.legacy) positionAt = {};
        return false;
    }
    const auto time = result.response.value("RelTime");
    unsigned long long h, m, s; char tail;
    if (sscanf(time.c_str(), "%llu:%llu:%llu%c", &h, &m, &s, &tail) != 3 || m >= 60 || s >= 60
        || h > std::numeric_limits<uint32_t>::max() / 3600000u
        || (h * 3600 + m * 60 + s) * 1000 > std::numeric_limits<uint32_t>::max()) return false;
    if (timing_probe::enabled()) timing_probe::diagnostics().seed(probeContext,double(h*3600+m*60+s),(probeStart+timing_probe::clockSeconds())/2);
    StateUpdate update; update.startedRevision = before.revision;
    update.positionMs = (h * 3600 + m * 60 + s) * 1000; update.positionText = time;
    if (result.response.child("TrackDuration")) update.duration = result.response.value("TrackDuration");
    XmlNode metadata;
    if (parseXml(result.response.value("TrackMetaData"), metadata) && metadata.child("item"))
        update.title = metadata.child("item")->value("title");
    apply(update); return cached();
}
bool OwnSpeakerControl::readTransportInfo(TransportInfo& info) {
    StateUpdate update; update.startedRevision = state.snapshot().revision;
    const auto result = call("GetTransportInfo", {{"InstanceID", "0"}});
    update.available = result.ok && result.response.child("CurrentTransportState") && result.response.child("CurrentTransportStatus");
    if (*update.available) { update.state = result.response.value("CurrentTransportState"); update.status = result.response.value("CurrentTransportStatus"); }
    apply(update); info = state.snapshot().transport; return info.available;
}
void OwnSpeakerControl::poll() {
    const auto health = subscriptions ? subscriptions->health() : SubscriptionHealth{};
    const auto activity = streamActivity ? streamActivity() : StreamActivity{};
    MonitorWork work;
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        std::string description;
        for (size_t i = 0; i < health.active.size(); ++i) if (monitor.legacy || !health.active[i])
            description += std::string("yeney: monitor polling ") + serviceName(static_cast<Service>(i)) + " reason="
                + (monitor.legacy ? "legacy" : health.reason[i]) + "\n";
        if (description.empty()) description = "yeney: monitor events\n";
        if (description != monitorLog) { printf("%s", description.c_str()); monitorLog = description; }
        work = monitor.next(Clock::now(), state.snapshot(), health.active, activity.requestOpen);
    }
    if (work.transport) { TransportInfo info; readTransportInfo(info); }
    // Recheck the freshly observed state; positionInfo enforces its own one-second lease.
    if (work.position || state.snapshot().playing()) { uint32_t ms; positionInfo(ms); }
    if (work.mediaDue && (!state.snapshot().paused() || (monitor.stoppedMediaInfo && !activity.requestOpen))) {
        std::string uri; currentUri(uri);
    }
    if (work.volume) {
        StateUpdate update; update.startedRevision = state.snapshot().revision;
        auto result = call("GetVolume", {{"InstanceID", "0"}, {"Channel", "Master"}}, "", "RenderingControl");
        const auto value = result.response.value("CurrentVolume"); unsigned parsed; char tail;
        if (result.ok && sscanf(value.c_str(), "%u%c", &parsed, &tail) == 1 && parsed <= 100) { update.volume = parsed; apply(update); }
    }
    if (work.topology) topology(speaker().ip, false);
}
}
