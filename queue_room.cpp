// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room.h"
#include "core_queue_sink.h"
#include "queue_path_sink.h"
#include "queue_playlist.h"
#include "queue_protocol.h"
#include "queue_route.h"
#include "queue_source.h"
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
#include "lms_icon.h"
extern "C" void yeney_queue_stream_handoff();
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
    std::function<std::string()> streamUrl;
    std::shared_ptr<upnp::OwnSpeakerControl> speaker;
    queue_audio::CoreQueueSink queue;
    QueuePathSink paths;
    std::atomic_bool selectionDeferred{false};
    std::atomic<int> metadataIndex{-1};
    int selectedIndex = -1;
    bool selecting = false, selectionKnown = false, selectedLocal = true, stoppedPlay = true;
    bool selectedNewPlay = true;
    yeney::Command selectedCommand{'s', 0, false}, requestedCommand{'s', 0, false};
    std::atomic<uint64_t> selectionRequest{0};
    uint64_t selectedRequest = 0;
    queue_audio::PlaylistEntry selectedTrack;
    std::map<uint64_t, queue_audio::PlaylistEntry> sourceTracks;
    std::map<std::string, yeney::FlacSourceInfo> flacCounts;
    std::chrono::steady_clock::time_point handoffAt{};
    std::string pathReason = "local-source";
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
        std::string sourceUrl;
        uint64_t frames;
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
    bool waitingForPlay = false;
    bool externalReleased = false, stopAcknowledged = false, sourceAway = false;
    std::atomic<int64_t> resumeEchoUntil{0};
    std::atomic_bool resumeFlush{false};
    std::chrono::steady_clock::time_point deferredStop{};
    uint64_t deferredNext = 0;
    queue_audio::PlaylistSnapshot resumeSnapshot;
    static int64_t milliseconds() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
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
        if (command == "pause 0" && current == queue.latestOccurrence() && queue.queueItemProduced(current)) {
            try {
                resumeEchoUntil = 0;
                resumeSnapshot = reader.read(host, mac);
                resumeEchoUntil = milliseconds() + 3000;
            } catch (const std::exception &e) {
                logQueue("resume echo snapshot unavailable: " + std::string(e.what()));
            }
        }
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
        resumeEchoUntil = 0;
        resumeFlush = false;
        deferredStop = {};

        externalReleased = true;
        stopAcknowledged = false;
        firstStream = queue.latestOccurrence() + 1;
        // No Stop/URI switch here: another service may already own transport.
        try {
            release();
        } catch (const std::exception &e) {
            fault = "external-release: " + std::string(e.what());
            started = waitingForPlay = false;
            current = 0;
            state = "STOPPED";
            logQueue(fault);
            publish();
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
         std::shared_ptr<upnp::OwnSpeakerControl> s, std::string h, std::string m, const uint8_t *rawMac,
         std::function<std::string()> url)
        : room(std::move(r)), host(std::move(h)), mac(std::move(m)), session(streamSessionToken()),
          streamUrl(std::move(url)), speaker(std::move(s)),
          queue(room.name, queue_audio::Config::environment(), 60), paths(queue, rawMac),
          route(room.name, session), coordinator(room, logQueue),
          actions([this](const auto &name, const auto &args) {
              if (sourceAway && (name == "Stop" || name == "Play" || name == "Pause" || name == "Seek")) {
                  logQueue(name + " skipped: confirmed transport source is not the room queue");
                  upnp::SoapResult result;
                  result.ok = true;
                  return result;
              }
              if (name == "Play" || name == "Pause" || name == "Stop" || name == "Seek" ||
                  name == "SetAVTransportURI")
                  ownUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
              auto result =
                  releasing ? coordinator.releaseTransport(name, args) : coordinator.transport(name, args);
              if (name == "SetAVTransportURI" && result.ok)
                  sourceAway = false;
              return result;
          }),
          ownership(ownershipIO(), session) {
        auto colon = host.find(':');
        if (colon != std::string::npos)
            host.resize(colon);
        route.registerWith(*http);
        relayEvents(true);
        paths.describeSource = [this](uint64_t id) {
            std::lock_guard<std::mutex> lock(sourceMutex);
            sourceTracks[id] = selectedTrack;
        };
        logQueue("settings poll_ms=" + std::to_string(pollMs));
        queue.useSpeakerClock();
        setRoomStatus(&status);
        publish();
        queue.onQueueCommand([this](char c, uint32_t) {
            if (milliseconds() < resumeEchoUntil && (c == 'q' || (c == 's' && resumeFlush))) {
                std::lock_guard<std::mutex> lock(mutex);
                if (c == 'q') {
                    resumeFlush = true;
                    commands.push_back({'F', queue.latestOccurrence() + 1});
                } else {
                    resumeEchoUntil = 0;
                    resumeFlush = false;
                    commands.push_back({'C', queue.latestOccurrence() + 1});
                }
                changed.notify_all();
                return true; // Keep immutable audio until the paired start is verified.
            }
            if (c == 'q' || c == 's' || c == 'p' || c == 'u' || c == 'f') {
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
    std::optional<yeney::FlacSourceInfo> sourceInfo(uint64_t occurrence,
                                                    std::optional<yeney::FlacSourceInfo> info) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            auto found = sourceTracks.find(occurrence);
            if (found == sourceTracks.end())
                return info;
            id = found->second.id;
            if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
                return info;
            if (info) {
                flacCounts[id] = *info;
                return info;
            }
            auto cached = flacCounts.find(id);
            if (cached != flacCounts.end())
                return cached->second;
        }
        // Metadata only, never PCM/audio from this request. Headerless native
        // seeks retain the original source's frame number, not a Range offset.
        auto result = upnp::httpRequest("GET", {host, "/music/" + id + "/download", 9000},
                                        {{"Range", "bytes=0-8191"}}, {}, 700);
        if ((result.status != 200 && result.status != 206) || !result.error.empty() ||
            result.body.size() < 42)
            return std::nullopt;
        auto p = reinterpret_cast<const unsigned char *>(result.body.data());
        if (std::string(result.body.data(), 4) != "fLaC" || (p[4] & 127) || p[5] || p[6] || p[7] != 34)
            return std::nullopt;
        yeney::FlacSourceInfo fetched;
        fetched.fixedBlockSize = unsigned(p[10]) * 256 + p[11];
        fetched.totalSamples = p[21] & 15;
        for (unsigned n = 22; n < 26; ++n)
            fetched.totalSamples = (fetched.totalSamples << 8) | p[n];
        if (!fetched.totalSamples || fetched.fixedBlockSize < 16)
            return std::nullopt;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            flacCounts[id] = fetched;
        }
        logQueue("native FLAC exact metadata track_id=" + id +
                 " source_frames=" + std::to_string(fetched.totalSamples));
        return fetched;
    }
    void relayEvents(bool enabled) {
        if (!enabled) {
            speaker->queueEvents({});
            return;
        }
        speaker->queueEvents([dirty = eventDirty, changes = speakerChanges](const upnp::GenaEvent &event) {
            if (event.service == upnp::Service::AVTransport) {
                changes->event(event);
                *dirty = true;
            }
            return true;
        });
    }
    void received(const yeney::Command &c) {
        if (c.letter == 's') {
            if (milliseconds() < resumeEchoUntil && resumeFlush) {
                selectionDeferred = false;
                return;
            }
            if (!selectionDeferred.exchange(true)) {
                std::lock_guard<std::mutex> lock(mutex);
                requestedCommand = c;
                commands.push_back({'I', ++selectionRequest});
                changed.notify_all();
            }
        } else {
            if (c.letter == 'q') {
                std::lock_guard<std::mutex> lock(mutex);
                ++selectionRequest;
                paths.cancelSelection();
                selectionDeferred = false;
                if (!paths.isRelay()) {
                    commands.push_back({'Q', 0});
                    changed.notify_all();
                }
            }
            paths.command(c);
        }
    }
    void beginSelection(uint64_t request) {
        std::lock_guard<std::mutex> lock(mutex);
        if (request != selectionRequest)
            return;
        selectedRequest = request;
        selectedCommand = requestedCommand;
        selecting = true;
        selectionKnown = false;
        selectedNewPlay = stoppedPlay || state == "STOPPED";
    }
    void selectPath() {
        if (!selecting || selectedRequest != selectionRequest)
            return;
        if (!selectionKnown) {
            queue_audio::PlaylistEntry track;
            selectedIndex = -1;
            try {
                playlist = reader.read(host, mac);
                const auto index = playlist.index + (selectedNewPlay ? 0 : 1);
                if (index < playlist.entries.size()) {
                    track = playlist.entries[index];
                    selectedIndex = int(index);
                }
            } catch (const std::exception &e) {
                logQueue("source metadata unavailable; using stream: " + std::string(e.what()));
            }
            if (selectedRequest != selectionRequest) {
                selecting = false;
                return;
            }
            selectedLocal = !remotePlaybackSource(track.id, track.remote, track.url);
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                selectedTrack = track;
            }
            selectionKnown = true;
            if (!selectedNewPlay && selectedLocal != paths.isRelay()) {
                paths.waitBoundary();
                logQueue("path-pending path=" + std::string(selectedLocal ? "relay" : "stream") +
                         " waiting=audible-end");
            }
        }
        const bool switching = selectedLocal != paths.isRelay();
        if (switching && !selectedNewPlay && !paths.atBoundary())
            return;
        if (switching) {
            if (!selectedNewPlay)
                handoffAt = std::chrono::steady_clock::now();
            if (paths.isRelay()) {
                if (ownership.active())
                    actions.stop();
                release();
                queue.flush();
                queue.waitIdle();
            } else {
                yeney_queue_stream_handoff();
                speaker->stop();
            }
            relayEvents(selectedLocal);
        }
        bool continues = false;
        if (selectedIndex >= 0 && size_t(selectedIndex + 1) < playlist.entries.size()) {
            const auto &next = playlist.entries[size_t(selectedIndex + 1)];
            continues = (!remotePlaybackSource(next.id, next.remote, next.url)) == selectedLocal;
        }
        paths.mayContinue(continues);
        metadataIndex = !selectedLocal && !selectedNewPlay ? selectedIndex : -1;
        pathReason = selectedLocal ? "local-source" : "remote-source";
        logQueue("path=" + std::string(selectedLocal ? "relay" : "stream") + " reason=" + pathReason +
                 " boundary=" + (selectedNewPlay ? "new-play" : "track-end"));
        if (selectedLocal)
            command('s', queue.latestOccurrence() + 1);
        // Core resets only at a class boundary, without HELO or an LMS command.
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (selectedRequest != selectionRequest) {
                selecting = false;
                return;
            }
            paths.select(selectedLocal, switching, selectedCommand);
            selectionDeferred = false;
        }
        stoppedPlay = false;
        selecting = selectionKnown = false;
        publish();
    }
    void handoffComplete() {
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - handoffAt)
                             .count();
        logQueue("path-handoff path=" + std::string(paths.isRelay() ? "relay" : "stream") +
                 " gap_ms=" + std::to_string(gap));
        handoffAt = {};
    }
    void publish() {
        ModeStatus v;
        v.requested = v.active = "queue";
        v.reason = pathReason;
        v.path = pathReason == "remote-source" ? "stream" : "relay";
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
        if (c == 'f') {
            ownUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            return;
        }
        if (c == 'F') {
            deferredNext = next;
            deferredStop = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            return;
        }
        if (c == 'C') {
            deferredStop = {};
            queue_audio::PlaylistSnapshot snapshot;
            bool known = false;
            try {
                snapshot = reader.read(host, mac);
                known = true;
            } catch (const std::exception &e) {
                logQueue("resume echo verification unavailable: " + std::string(e.what()));
            }
            const auto audible =
                std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.stream == current; });
            const bool same =
                known && audible != items.end() && snapshot.index == resumeSnapshot.index &&
                snapshot.index < snapshot.entries.size() && snapshot.index < resumeSnapshot.entries.size() &&
                snapshot.entries[snapshot.index].url == audible->sourceUrl &&
                snapshot.entries[snapshot.index].url == resumeSnapshot.entries[snapshot.index].url &&
                std::abs(int64_t(snapshot.timeMs) - int64_t(resumeSnapshot.timeMs)) <= 500;
            if (same) {
                queue.restartCached();
                queue.confirmCachedStart();
                fastPoll();
                logQueue("resume echo adopted: same LMS item/time, retained audio, no renderer restart");
            } else {
                queue.flush();
                command('q', next);
                queue.resumeReceivedStart();
                logQueue("resume echo changed item/time: honour LMS selection");
            }
            return;
        }
        if (c == 'q') {
            handoffAt = {};
            if (externalReleased)
                stopAcknowledged = true;
            expectedState = "STOPPED";
            selecting = selectionKnown = false;
            stoppedPlay = true;
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
        } else if (c == 'u' && started) {
            expectedState = "PLAYING";

            if (state == "PAUSED" && !sourceAway) {
                actions.play();
                playAt = std::chrono::steady_clock::now();
                waitingForPlay = true;
                fastPoll();
            }
        }
    }
    void append() {
        if (!paths.isRelay() || faulted || externalReleased || (started && sourceAway))
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
            std::optional<uint64_t> length;
            try {
                length = record.item->contentLength();
            } catch (const queue_audio::ItemCancelled &) {
                continue;
            }
            if (!length)
                continue;
            if (!record.fault.empty())
                throw std::runtime_error(record.fault);
            queue_audio::PlaylistEntry track;
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                auto found = sourceTracks.find(record.source.occurrence);
                if (found == sourceTracks.end())
                    throw std::runtime_error("queue-stream-metadata-missing");
                track = found->second;
                remote[record.source.occurrence] = false;
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
            try {
                metadata.url = speaker->controllerUri() + route.publish(room.name, 1, 1, record.item);
            } catch (const queue_audio::ItemCancelled &) {
                continue;
            }
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
            items.push_back(
                {record.source.occurrence, metadata.url, record.item, track.url, metadata.frames});
            publish();
            if (!started) {
                actions.selectQueue(room.uuid);
                actions.seekTrack(1);
                expectedState = "PLAYING";

                actions.play();
                playAt = std::chrono::steady_clock::now();
                waitingForPlay = true;
                fastPoll();
                started = true;
            }
        }
    }
    void observe() {
        if (!paths.isRelay()) {
            auto info = speaker->transportInfo();
            std::string uri;
            if (handoffAt != std::chrono::steady_clock::time_point{}) {
                if (!speaker->readTransportInfo(info) || !speaker->currentUri(uri))
                    return;
                if (uri != streamUrl())
                    return;
            }
            const auto next = info.state == "PLAYING"           ? "PLAYING"
                              : info.state == "PAUSED_PLAYBACK" ? "PAUSED"
                                                                : "STOPPED";
            if (state != next) {
                state = next;
                publish();
            }
            if (state == "PLAYING" && handoffAt != std::chrono::steady_clock::time_point{})
                handoffComplete();
            return;
        }
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
        const auto uri = p.value("TrackURI");
        const auto avUri = media.value("CurrentURI");
        if (media.child("CurrentURI"))
            sourceAway = !avUri.empty() && avUri != "x-rincon-queue:" + room.uuid + "#0";
        if (!uri.empty() &&
            std::none_of(items.begin(), items.end(), [&](const auto &i) { return i.uri == uri; }))
            sourceAway = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!commands.empty())
                return; // SlimProto arrived while SOAP was in flight.
        }
        const auto transport = info.value("CurrentTransportState");
        const auto reportedStatus = info.value("CurrentTransportStatus");
        if (reportedStatus != transportStatus) {
            transportStatus = reportedStatus;
            logQueue("TransportStatus=" + transportStatus + " uri=" + uri);
        }
        auto it = std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.uri == uri; });
        if (sourceAway) {
            if (now >= ownUntil)
                externalStop("speaker-source-change");
            return; // Neither an error nor the echo guard authorises stopping a foreign source.
        }
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
        bool cancelledCurrent = false;
        const auto checked = it != items.end() ? it : previousItem;
        if (checked != items.end()) {
            try {
                (void)checked->item->contentLength();
            } catch (const queue_audio::ItemCancelled &) {
                cancelledCurrent = true;
            }
        }
        unsigned ph = 0, pm = 0, ps = 0;
        const bool positionValid = sscanf(p.value("RelTime").c_str(), "%u:%u:%u", &ph, &pm, &ps) == 3;
        const uint64_t reportedElapsed = positionValid ? uint64_t(ph * 3600 + pm * 60 + ps) : 0;
        const auto endEvidence = it == previousItem ? std::max(lastElapsed, reportedElapsed) : lastElapsed;
        const bool nearEnd =
            finalItem && previousItem->frames <= (endEvidence + 2) * previousItem->item->descriptor().rate;
        const auto source = media.value("CurrentURI");
        const bool sourceChanged = media.child("CurrentURI") &&
                                   source != "x-rincon-queue:" + room.uuid + "#0" &&
                                   (!source.empty() || !nearEnd);
        const bool missingItem = it == items.end();
        const bool unsolicited = now >= ownUntil && !waitingForPlay;
        if (unsolicited && expectedState != "STOPPED" &&
            (sourceChanged || (!nearEnd && (transport == "STOPPED" || missingItem)))) {
            if (cancelledCurrent && !sourceChanged)
                throw std::runtime_error("queue cancelled item stopped after LMS flush");
            externalStop(sourceChanged ? "speaker-source-change" : "speaker-stop");
            return;
        }
        std::string mutation;
        if (unsolicited && expectedState == "PLAYING" && transport == "PAUSED_PLAYBACK") {

            expectedState = "PAUSED";
            mutation = "pause 1";
        } else if (unsolicited && expectedState == "PAUSED" && transport == "PLAYING" && !missingItem) {

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
        const auto frames = it->frames;
        const auto rate = it->item->descriptor().rate;
        if (transport == "PLAYING" && frames <= (elapsed + 3) * rate)
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
        if (state == "PLAYING" && handoffAt != std::chrono::steady_clock::time_point{})
            handoffComplete();
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
                for (const auto &c : pending) {
                    if (c.first == 'I')
                        beginSelection(c.second);
                    else if (c.first == 'Q') {
                        selecting = selectionKnown = false;
                        handoffAt = {};
                        stoppedPlay = true;
                        state = "STOPPED";
                        publish();
                    } else
                        command(c.first, c.second);
                }
                selectPath();
                if (deferredStop != std::chrono::steady_clock::time_point{} &&
                    std::chrono::steady_clock::now() >= deferredStop) {
                    deferredStop = {};
                    resumeEchoUntil = 0;
                    resumeFlush = false;
                    queue.flush();
                    command('q', deferredNext);
                }
                observe();
                append();
            } catch (const queue_audio::ItemCancelled &) {
                // Producer cancellation raced a snapshot; observe the next run.
            } catch (const std::exception &e) {
                fault = e.what();
                faulted = true;
                logQueue("relay fault=" + fault);
                try {
                    if (!externalReleased)
                        actions.stop();
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
                     std::string mac, const uint8_t *rawMac, std::function<std::string()> url)
    : impl(new Impl(std::move(room), std::move(http), std::move(speaker), std::move(host), std::move(mac),
                    rawMac, std::move(url))) {}
QueueRoom::~QueueRoom() = default;
yeney::Sink &QueueRoom::sink() { return impl->paths; }
void QueueRoom::shutdown() { impl->shutdown(); }
void QueueRoom::command(const yeney::Command &c) { impl->received(c); }
bool QueueRoom::queueActive() const { return impl->paths.isRelay(); }
int QueueRoom::streamMetadataIndex() const { return impl->metadataIndex; }
bool QueueRoom::deferStart() { return impl->selectionDeferred; }
std::optional<yeney::FlacSourceInfo> QueueRoom::flacSourceInfo(uint64_t id,
                                                               std::optional<yeney::FlacSourceInfo> info) {
    return impl->sourceInfo(id, info);
}
bool QueueRoom::remoteSource(uint64_t id) const {
    std::lock_guard<std::mutex> lock(impl->sourceMutex);
    auto it = impl->remote.find(id);
    return it == impl->remote.end() || it->second;
}
void runQueueCoreClient(const char *server, const uint8_t *mac, const char *name, QueueRoom &room) {
    std::signal(SIGINT, stopRoom);
    std::signal(SIGTERM, stopRoom);
    LmsIcon icon(mac, [](const auto &line) { std::printf("%s\n", line.c_str()); });
    yeney::Config config;
    std::string host = server ? server : "";
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        config.port = std::stoul(host.substr(colon + 1));
        host.resize(colon);
    }
    config.server = host;
    config.name = name;
    config.connectionReady = [&icon](const auto &peer) { icon.connected(peer); };
    std::copy(mac, mac + 6, config.mac.begin());
    config.startOnSubmit = true;
    config.observeCommand = [&room](const auto &c) { room.command(c); };
    config.deferQueueStart = [&room] { return room.deferStart(); };
    config.queueNextDelayMs = queueNextDelayMs();
    config.flacSourceInfo = [&room](uint64_t id, auto info) { return room.flacSourceInfo(id, info); };
    config.queueRelativeClock = true;
    config.queueRemoteSource = [&room](uint64_t id) { return room.remoteSource(id); };
    config.log = [](const auto &line) { std::printf("core: %s\n", line.c_str()); };
    yeney::Player player(config, room.sink());
    player.run(roomStop);
    room.shutdown();
    icon.shutdown();
    std::exit(0);
}
} // namespace bridge
