// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room.h"
#include "core_queue_sink.h"
#include "queue_playlist.h"
#include "queue_protocol.h"
#include "queue_route.h"
#include "queue_speaker_changes.h"
#include "stream_metrics.h"
#include "stream_session.h"
#include "upnp/queue_coordinator.h"
#include "upnp/queue_metadata.h"
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <deque>
#include <set>
#include <thread>
namespace bridge {
namespace {
std::atomic_bool roomStop{false};
void stopRoom(int) { roomStop = true; }
void logQueue(const std::string &s) {
    std::printf("yeney: %s%s\n", s.compare(0, 6, "queue ") == 0 ? "" : "queue ", s.c_str());
}
unsigned pollSetting() {
    if (const char *text = std::getenv("YENEY_QUEUE_POLL_MS")) {
        char *end = nullptr;
        long n = std::strtol(text, &end, 10);
        if (*text && end && !*end && n >= 100 && n <= 2000)
            return unsigned(n);
        logQueue("invalid YENEY_QUEUE_POLL_MS; using 500");
    }
    return 500;
}
} // namespace
// Only this worker mutates the speaker or relay state. Core callbacks enqueue
// commands; HTTP readers serve immutable items and never advance playback.
struct QueueRoom::Impl {
    upnp::Speaker room;
    std::string host, mac, session;
    std::shared_ptr<upnp::OwnSpeakerControl> speaker;
    queue_audio::CoreQueueSink queue;
    queue_audio::Route route;
    upnp::QueueCoordinator coordinator;
    upnp::QueueActions actions;
    std::set<std::string> owned;
    upnp::QueueOwnership ownership;
    RoomStatus status;
    std::shared_ptr<SpeakerChanges> speakerChanges = std::make_shared<SpeakerChanges>();
    std::string publishedState, publishedUri;
    struct Entry {
        uint64_t stream;
        std::string uri;
        std::shared_ptr<queue_audio::Item> item;
    };
    std::vector<Entry> items;
    std::map<uint64_t, bool> remote;
    std::mutex mutex, sourceMutex;
    std::condition_variable changed;
    std::deque<std::pair<char, uint64_t>> commands;
    std::thread worker;
    bool closing = false, started = false, faulted = false, releasing = false;
    uint64_t firstStream = 1, current = 0;
    queue_audio::PlaylistSnapshot playlist;
    queue_audio::PlaylistReader reader;
    std::string state = "STOPPED", fault, transportStatus;
    std::chrono::steady_clock::time_point playAt{};
    bool waitingForPlay = false, externalPaused = false;
    bool externalReleased = false, stopAcknowledged = false;
    std::string expectedState = "STOPPED";
    std::chrono::steady_clock::time_point ownUntil{};
    uint64_t lastElapsed = 0;
    // The only outgoing CLI mutation path: called solely from speaker evidence.
    void speakerCommand(const std::string &command, const std::string &reason) {
        const bool allowed =
            (command == "stop" && (reason == "speaker-stop" || reason == "speaker-source-change")) ||
            (command == "pause 1" && reason == "speaker-pause") ||
            (command == "pause 0" && reason == "speaker-resume");
        if (!allowed)
            throw std::logic_error("invalid speaker command");
        logQueue("LMS mutation reason=" + reason + " command=" + command);
        if (!queue_audio::controlLms(host, mac, command)) {
            std::string reply;
            const bool verified = queue_audio::controlLms(host, mac, "mode ?", 9090, &reply, 3000);
            logQueue("LMS mutation uncertain command=" + command +
                     " verification=" + (verified ? queue_audio::decodeLms(reply) : "unavailable"));
        }
    }
    void externalStop(const std::string &reason) {
        expectedState = "STOPPED";
        externalPaused = false;
        externalReleased = true; stopAcknowledged = false;
        firstStream = queue.latestOccurrence() + 1;
        // No Stop/URI switch here: another service may already own transport.
        try { release(); }
        catch (const std::exception &e) {
            fault = "external-release: " + std::string(e.what());
            started = waitingForPlay = false; current = 0; state = "STOPPED";
            logQueue(fault); publish();
        }
        speakerCommand("stop", reason);
    }
    unsigned pollMs = pollSetting();
    std::chrono::steady_clock::time_point nextPoll{}, fastUntil{};
    std::shared_ptr<std::atomic_bool> eventDirty = std::make_shared<std::atomic_bool>(false);
    void fastPoll() {
        fastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        nextPoll = {};
    }
    upnp::QueueOwnershipIO ownershipIO() {
        upnp::QueueOwnershipIO io;
        io.browse = [this] { return coordinator.browse(); };
        io.owned = [this](const auto &uri) { return owned.count(uri) != 0; };
        io.playlists = [this] { return coordinator.playlists(); };
        io.save = [this](const auto &title) {
            return actions.save(title).response.value("AssignedObjectID");
        };
        io.deletePlaylist = [this](const auto &id) { coordinator.destroyPlaylist(id); };
        io.crossfade = [this] { return actions.crossfade().response.value("CrossfadeMode") == "1"; };
        io.setCrossfade = [this](bool value) {
            actions.crossfade(value);
            if (actions.crossfade().response.value("CrossfadeMode") != (value ? "1" : "0"))
                throw std::runtime_error("queue-crossfade-unconfirmed");
        };
        io.remove = [this](auto update, auto first, auto count) { actions.remove(update, first, count); };
        io.log = logQueue;
        return io;
    }
    Impl(upnp::Speaker r, std::shared_ptr<upnp::StreamServer> http,
         std::shared_ptr<upnp::OwnSpeakerControl> s, std::string h, std::string m)
        : room(std::move(r)), host(std::move(h)), mac(std::move(m)), session(streamSessionToken()),
          speaker(std::move(s)), queue(room.name, queue_audio::Config::environment(), 60),
          route(room.name, session), coordinator(room, logQueue),
          actions([this](const auto &name, const auto &args) {
              if (name == "Play" || name == "Pause" || name == "Stop" || name == "Seek" ||
                  name == "SetAVTransportURI")
                  ownUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
              return releasing ? coordinator.releaseTransport(name, args) : coordinator.transport(name, args);
          }),
          ownership(ownershipIO(), session) {
        auto colon = host.find(':');
        if (colon != std::string::npos)
            host.resize(colon);
        if (const char *directory = std::getenv("STATE_DIRECTORY"))
            streamMetrics().configure(room.name, directory);
        route.registerWith(*http);
        speaker->queueEvents([dirty = eventDirty, changes = speakerChanges](const upnp::GenaEvent &event) {
            if (event.service == upnp::Service::AVTransport) {
                changes->event(event);
                *dirty = true;
            }
            return true;
        });
        logQueue("settings poll_ms=" + std::to_string(pollMs));
        queue.useSpeakerClock();
        setRoomStatus(&status);
        publish();
        queue.onQueueCommand([this](char c, uint32_t) {
            if (c == 'q' || c == 's' || c == 'p' || c == 'u') {
                std::lock_guard<std::mutex> lock(mutex);
                commands.push_back({c, queue.latestOccurrence() + 1});
                changed.notify_all();
            }
            // q must perform the normal core producer reset and STMf. p/u
            // acknowledge only the speaker-confirmed pause/resume state.
            return c == 'p' || c == 'u';
        });
        worker = std::thread([this] { run(); });
    }
    void publish() {
        ModeStatus v;
        v.requested = v.active = "queue";
        v.reason = "queue-opt-in";
        v.state = state;
        v.owned = ownership.active();
        v.occurrence = current;
        v.lastFault = fault;
        for (const auto &item : items) {
            v.ownedUris.push_back(item.uri);
            if (item.stream == current) {
                v.currentUri = item.uri;
                v.renderedRate = item.item->descriptor().rate;
            }
        }
        if (publishedState != v.state || publishedUri != v.currentUri) {
            logQueue("relay-change state=" + publishedState + "->" + v.state + " current=" + publishedUri +
                     "->" + v.currentUri);
            publishedState = v.state;
            publishedUri = v.currentUri;
        }
        status.publish(std::move(v));
        // Bypass the periodic stream sampler's one-second throttle. Snapshot
        // publication happens in the same call as every relay state update.
        streamMetrics().publishPlayback();
    }
    void release() {
        releasing = true;
        try {
            ownership.release();
        } catch (...) {
            releasing = false;
            throw;
        }
        releasing = false;
        items.clear();
        started = false;
        waitingForPlay = false;
        current = 0;
        lastElapsed = 0;
        state = "STOPPED";
        publish();
    }
    void command(char c, uint64_t next) {
        if (c == 'q') {
            if (externalReleased) stopAcknowledged = true;
            expectedState = "STOPPED";
            externalPaused = false;
            firstStream = next;
            if (ownership.active() && !externalReleased)
                actions.stop();
            release();
            faulted = false;
            fault.clear();
            playlist = {};
            publish();
        } else if (c == 's' && externalReleased && stopAcknowledged) {
            externalReleased = false;
            firstStream = next;
            playlist = {};
        } else if (c == 's' && state == "STOPPED" && (started || faulted)) {
            if (started || faulted)
                release();
            firstStream = next;
            playlist = {};
            faulted = false;
            fault.clear();
            publish();
        } else if (c == 'p' && started) {
            expectedState = "PAUSED";
            if (state != "PAUSED")
                actions.pause();
        } else if (c == 'u' && started && state == "PAUSED") {
            expectedState = "PLAYING";
            externalPaused = false;
            actions.play();
            playAt = std::chrono::steady_clock::now();
            waitingForPlay = true;
            fastPoll();
        }
    }
    void append() {
        if (faulted || externalReleased)
            return;
        for (const auto &record : queue.snapshot()) {
            if (record.source.occurrence < firstStream || record.source.occurrence > UINT64_MAX / 2)
                continue;
            if (record.item && !record.incomplete.empty())
                continue; // LMS cancelled this render, not a speaker rejection.
            if (!record.fault.empty())
                throw std::runtime_error(record.fault);
            if (!record.item || !record.incomplete.empty())
                continue;
            if (std::any_of(items.begin(), items.end(),
                            [&](const auto &i) { return i.stream == record.source.occurrence; }))
                continue;
            if (record.source.rate > queue.maxSampleRate())
                throw std::runtime_error("queue-rate-unsupported rate=" + std::to_string(record.source.rate) +
                                         " cap=" + std::to_string(queue.maxSampleRate()));
            const auto length = record.item->contentLength();
            if (!length)
                continue;
            if (!record.fault.empty())
                throw std::runtime_error(record.fault);
            playlist = reader.read(host, mac);
            // LMS admits at most one successor after this audible stream's
            // STMd. Read its current index rather than retaining playlist
            // ordinals across stops, seeks or edits.
            const auto index = playlist.index + (started ? 1 : 0);
            if (index >= playlist.entries.size())
                throw std::runtime_error("queue-stream-metadata-missing");
            const auto &track = playlist.entries[index];
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                remote[record.source.occurrence] =
                    track.remote == "1" || track.url.compare(0, 7, "file://") != 0;
            }
            if (!ownership.active()) {
                auto now = std::time(nullptr);
                std::tm tm{};
                localtime_r(&now, &tm);
                char title[64];
                std::strftime(title, sizeof title, "Before YeneY %Y-%m-%d %H:%M", &tm);
                ownership.acquire(title);
                logQueue("ownership acquired");
                publish();
            }
            const auto before = coordinator.browse();
            std::vector<std::string> expected;
            for (const auto &i : items)
                expected.push_back(i.uri);
            if (before.uris != expected)
                throw std::runtime_error("queue-changed-while-owned");
            upnp::QueueMetadata metadata;
            // Fixed legacy route namespace, not a playback epoch. Stream
            // occurrence IDs are unique for the lifetime of this connection.
            metadata.url = speaker->controllerUri() + route.publish(room.name, 1, 1, record.item);
            metadata.session = session;
            metadata.title = track.title;
            metadata.artist = track.artist;
            metadata.album = track.album;
            metadata.artwork = track.artwork(host);
            metadata.size = *length;
            metadata.frames = (*length - 44) / 6;
            metadata.rate = record.source.rate;
            const auto changes = upnp::sanitizeXmlFields({{"title", &metadata.title},
                                                          {"artist", &metadata.artist},
                                                          {"album", &metadata.album},
                                                          {"artwork", &metadata.artwork}});
            if (!changes.empty())
                logQueue("metadata sanitized mode=queue occurrence=" +
                         std::to_string(record.source.occurrence) + " " + changes);
            owned.insert(metadata.url); // include an enqueue whose reply is lost
            const auto result = actions.add(metadata.url, upnp::queueDidl(metadata));
            expected.push_back(metadata.url);
            const auto after = coordinator.browse();
            if (result.response.value("NumTracksAdded") != "1" || after.uris != expected ||
                after.update == before.update)
                throw std::runtime_error("queue-enqueue-unconfirmed");
            items.push_back({record.source.occurrence, metadata.url, record.item});
            publish();
            if (!started) {
                actions.selectQueue(room.uuid);
                actions.seekTrack(1);
                expectedState = "PLAYING";
                externalPaused = false;
                actions.play();
                playAt = std::chrono::steady_clock::now();
                waitingForPlay = true;
                fastPoll();
                started = true;
            }
        }
    }
    void observe() {
        if (!started || faulted)
            return;
        const auto now = std::chrono::steady_clock::now();
        const bool event = eventDirty->exchange(false);
        if (!event && now < nextPoll)
            return;
        nextPoll = now + std::chrono::milliseconds(now < fastUntil ? 100 : pollMs);
        const auto p = actions.position().response;
        const auto info = coordinator.transport("GetTransportInfo", {{"InstanceID", "0"}}).response;
        const auto media = coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response;
        speakerChanges->report("poll", {{"TransportState", info.value("CurrentTransportState")},
                                        {"TransportStatus", info.value("CurrentTransportStatus")},
                                        {"CurrentTrack", p.value("Track")},
                                        {"CurrentTrackURI", p.value("TrackURI")},
                                        {"NumberOfTracks", media.value("NrTracks")},
                                        {"AVTransportURI", media.value("CurrentURI")}});
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!commands.empty())
                return; // SlimProto arrived while SOAP was in flight.
        }
        const auto transport = info.value("CurrentTransportState");
        const auto reportedStatus = info.value("CurrentTransportStatus");
        const auto uri = p.value("TrackURI");
        if (reportedStatus != transportStatus) {
            transportStatus = reportedStatus;
            logQueue("TransportStatus=" + transportStatus + " uri=" + uri);
        }
        auto it = std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.uri == uri; });
        if (!reportedStatus.empty() && reportedStatus != "OK")
            throw std::runtime_error("playback-fault action=Play uri=" + uri +
                                     " TransportStatus=" + reportedStatus);
        if (waitingForPlay && transport == "STOPPED" &&
            std::chrono::steady_clock::now() - playAt > std::chrono::seconds(3))
            throw std::runtime_error("playback-fault action=Play uri=" + uri +
                                     " TransportStatus=" + reportedStatus + " stopped-after-play");
        const auto previousItem =
            std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.stream == current; });
        const bool finalItem = previousItem != items.end() && previousItem == std::prev(items.end());
        const auto previousLength =
            previousItem == items.end() ? std::optional<uint64_t>{} : previousItem->item->contentLength();
        unsigned ph = 0, pm = 0, ps = 0;
        const bool positionValid = sscanf(p.value("RelTime").c_str(), "%u:%u:%u", &ph, &pm, &ps) == 3;
        const uint64_t reportedElapsed = positionValid ? uint64_t(ph * 3600 + pm * 60 + ps) : 0;
        const auto endEvidence = it == previousItem ? std::max(lastElapsed, reportedElapsed) : lastElapsed;
        const bool nearEnd =
            finalItem && previousLength &&
            (*previousLength - 44) / 6 <= (endEvidence + 2) * previousItem->item->descriptor().rate;
        const auto source = media.value("CurrentURI");
        const bool sourceChanged = media.child("CurrentURI") &&
                                   source != "x-rincon-queue:" + room.uuid + "#0" &&
                                   (!source.empty() || !nearEnd);
        const bool missingItem = it == items.end();
        const bool unsolicited = now >= ownUntil && !waitingForPlay;
        if (unsolicited && (expectedState == "PLAYING" || externalPaused) &&
            (sourceChanged || (!nearEnd && (transport == "STOPPED" || missingItem)))) {
            externalStop(sourceChanged ? "speaker-source-change" : "speaker-stop");
            return;
        }
        std::string mutation;
        if (unsolicited && expectedState == "PLAYING" && transport == "PAUSED_PLAYBACK") {
            externalPaused = true;
            expectedState = "PAUSED";
            mutation = "pause 1";
        } else if (unsolicited && externalPaused && transport == "PLAYING" && !missingItem) {
            externalPaused = false;
            expectedState = "PLAYING";
            mutation = "pause 0";
        }
        if (missingItem && nearEnd && transport == "STOPPED")
            it = previousItem;
        if (it == items.end())
            return;
        if (transport == "PLAYING")
            waitingForPlay = false;
        if (waitingForPlay && transport == "STOPPED")
            return;
        unsigned h = 0, m = 0, s = 0;
        if (sscanf(p.value("RelTime").c_str(), "%u:%u:%u", &h, &m, &s) != 3)
            return;
        const auto playback = transport == "PLAYING"           ? yeney::PlaybackState::Playing
                              : transport == "PAUSED_PLAYBACK" ? yeney::PlaybackState::Paused
                                                               : yeney::PlaybackState::Stopped;
        if (playback == yeney::PlaybackState::Stopped && it != std::prev(items.end()))
            return;
        const auto elapsed =
            transport == "STOPPED" && nearEnd ? endEvidence : uint64_t(h * 3600 + m * 60 + s);
        if (transport != "STOPPED")
            lastElapsed = elapsed;
        const auto length = it->item->contentLength();
        const auto rate = it->item->descriptor().rate;
        if (transport == "PLAYING" && length && (*length - 44) / 6 <= (elapsed + 3) * rate)
            fastPoll();
        if (it->stream != current)
            fastPoll();
        if (transport == "STOPPED" && nearEnd)
            expectedState = "STOPPED";
        const auto previous = current;
        const auto old = state;
        current = it->stream;
        state = playback == yeney::PlaybackState::Playing  ? "PLAYING"
                : playback == yeney::PlaybackState::Paused ? "PAUSED"
                                                           : "STOPPED";
        queue.speakerPosition(current, elapsed * it->item->descriptor().rate, playback);
        if (current != previous || state != old) {
            publish();
            logQueue("state=" + state + (state == "PLAYING" ? " audible occurrence=" : " occurrence=") +
                     std::to_string(current));
        }
        if (!mutation.empty())
            speakerCommand(mutation, mutation == "pause 1" ? "speaker-pause" : "speaker-resume");
    }
    void run() {
        for (;;) {
            std::deque<std::pair<char, uint64_t>> pending;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(100),
                                 [&] { return closing || !commands.empty(); });
                if (closing)
                    break;
                pending.swap(commands);
            }
            try {
                for (const auto &c : pending)
                    command(c.first, c.second);
                observe();
                append();
            } catch (const std::exception &e) {
                fault = e.what();
                faulted = true;
                logQueue("relay fault=" + fault);
                try {
                    if (!externalReleased) actions.stop();
                } catch (const std::exception &stop) {
                    logQueue(stop.what());
                }
                try {
                    release();
                } catch (const std::exception &cleanup) {
                    logQueue(cleanup.what());
                }
                queue.failQueueTrack();
                publish();
            }
        }
        try {
            if (ownership.active() && !externalReleased)
                actions.stop();
            release();
        } catch (const std::exception &e) {
            logQueue("release failed=" + std::string(e.what()));
        }
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closing)
                return;
            closing = true;
        }
        changed.notify_all();
        worker.join();
        speaker->queueEvents({});
        setRoomStatus(nullptr);
    }
    ~Impl() { shutdown(); }
};
QueueRoom::QueueRoom(upnp::Speaker room, std::shared_ptr<upnp::StreamServer> http,
                     std::shared_ptr<upnp::OwnSpeakerControl> speaker, PlaybackSelection, std::string host,
                     std::string mac, const uint8_t *, std::function<std::string()>)
    : impl(new Impl(std::move(room), std::move(http), std::move(speaker), std::move(host), std::move(mac))) {}
QueueRoom::~QueueRoom() = default;
yeney::Sink &QueueRoom::sink() { return impl->queue; }
void QueueRoom::shutdown() { impl->shutdown(); }
void QueueRoom::command(const yeney::Command &) {}
bool QueueRoom::queueActive() const { return true; }
bool QueueRoom::deferStart() { return false; }
bool QueueRoom::remoteSource(uint64_t id) const {
    std::lock_guard<std::mutex> lock(impl->sourceMutex);
    auto it = impl->remote.find(id);
    return it == impl->remote.end() || it->second;
}
void runQueueCoreClient(const char *server, const uint8_t *mac, const char *name, QueueRoom &room) {
    std::signal(SIGINT, stopRoom);
    std::signal(SIGTERM, stopRoom);
    yeney::Config config;
    std::string host = server ? server : "";
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        config.port = std::stoul(host.substr(colon + 1));
        host.resize(colon);
    }
    config.server = host;
    config.name = name;
    std::copy(mac, mac + 6, config.mac.begin());
    config.startOnSubmit = true;
    config.queueNextDelayMs = queueNextDelayMs();
    config.queueRelativeClock = true;
    config.queueRemoteSource = [&room](uint64_t id) { return room.remoteSource(id); };
    config.log = [](const auto &line) { std::printf("core: %s\n", line.c_str()); };
    yeney::Player player(config, room.sink());
    player.run(roomStop);
    room.shutdown();
    std::exit(0);
}
} // namespace bridge
