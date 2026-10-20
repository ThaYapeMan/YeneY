// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room.h"
#include "core_queue_sink.h"
#include "core_sonos.h"
#include "mode_sink.h"
#include "queue_controller.h"
#include "queue_lms_client.h"
#include "queue_playlist.h"
#include "queue_protocol.h"
#include "stream_session.h"
#include "upnp/queue_coordinator.h"
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <set>
#include <thread>
namespace bridge {
namespace {
std::atomic_bool roomStop{false};
void stopRoom(int sig) {
    roomStop = true;
    std::signal(sig, SIG_DFL);
}
uint64_t mono() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void logger(const std::string &s) { printf("yeney: %s\n", s.c_str()); }
} // namespace
struct QueueRoom::Impl {
    upnp::Speaker room;
    std::shared_ptr<upnp::OwnSpeakerControl> speaker;
    std::string host, mac;
    std::function<std::string()> streamUri;
    queue_audio::CoreQueueSink queue;
    CoreSonosSink stream;
    ModeSink router;
    RoomMode mode;
    std::unique_ptr<queue_audio::LmsClient> lms;
    std::unique_ptr<queue_audio::QueueController> controller;
    queue_audio::PlaylistReader reader;
    queue_audio::PlaylistSnapshot playlist;
    struct Binding {
        uint64_t token, generation;
        uint32_t origin;
    };
    std::map<uint64_t, Binding> bindings;
    mutable std::mutex sourceMutex;
    std::map<uint64_t, bool> remoteSources;
    std::map<uint64_t, queue_audio::QueueAsset> assets;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> jobs;
    std::thread worker;
    bool closing = false, drainRequested = false, queueReadyPending = false;
    std::atomic<bool> playing{false};
    std::atomic<uint64_t> epoch{1}, lastBound{0};
    uint64_t lastSnapshot = 0, lastPoll = 0;
    std::optional<unsigned> initialIndex;
    uint64_t producerToken = 0, selectionToken = 0;
    std::atomic<uint64_t> coreReady{0};
    std::atomic<bool> stopped{false}, readiness{true}, deferIncoming{false};
    std::atomic<uint64_t> queueGeneration{1};
    std::atomic<uint64_t> seekOrigin{UINT64_MAX}, pendingRender{0}, notifiedSeek{UINT64_MAX},
        ownSeek{UINT64_MAX};
    std::atomic<bool> refetchPending{false}, manualIntent{false}, renderPending{false},
        renderAdmission{false};
    std::mutex preparedMutex;
    std::map<uint64_t, Binding> prepared;
    Impl(upnp::Speaker r, std::shared_ptr<upnp::StreamServer> http,
         std::shared_ptr<upnp::OwnSpeakerControl> s, PlaybackSelection selection, std::string h,
         std::string m, const uint8_t *bytes, std::function<std::string()> uri)
        : room(std::move(r)), speaker(std::move(s)), host(std::move(h)), mac(std::move(m)),
          streamUri(std::move(uri)), queue(room.name, queue_audio::Config::environment(), 60), stream(bytes),
          router(queue, stream),
          mode(room.name, room.coordinator, selection,
               {[this](const ModeEvidence &e) {
                    if (controller)
                        controller->freeze(e.epoch).get();
                },
                [this](const ModeEvidence &) {
                    if (controller)
                        controller->release("mode-handoff").get();
                },
                [this](uint64_t g, const ModeEvidence &) { router.select(false, g); }, logger}) {
        auto colon = host.find(':');
        if (colon != std::string::npos)
            host.resize(colon);
        queue.useSpeakerClock();
        queue.onQueueCommand([this](char c, uint32_t) {
            if (c == 'f' && queue.latestOccurrence() != queue.audibleOccurrence()) {
                // LMS flushes a removed prefetch independently of the audible
                // song. A producer cancellation is not a transport Stop.
                manualIntent = false;
                logger("queue LMS prefetch flush: audible item retained");
                return true;
            }
            if (c == 'f' && renderAdmission)
                return true; // CLI re-render echo must preserve the prepared target
            if (c == 'f')
                manualIntent = !refetchPending;
            if (c == 'q' && playing)
                manualIntent = true;
            if (c == 'p' || c == 'u' || c == 'q' || c == 'f') {
                post([this, c] {
                    if (playing)
                        controller->lmsCommand(epoch, c).get();
                });
                return true;
            }
            if (c == 's' && renderAdmission.exchange(false)) {
                manualIntent = false;
                refetchPending = false;
                return false; // the requested re-render must reach the decoder
            }
            if (c == 's' && refetchPending.exchange(false)) {
                manualIntent = false;
                post([this] {
                    if (playing) {
                        queue.restartCached();
                        controller->lmsCommand(epoch, 's');
                        queue.confirmCachedStart();
                    }
                });
                return true;
            }
            if (c == 's' && manualIntent.exchange(false)) {
                post([this] { selectSnapshot(); });
                return true;
            }
            return false;
        });
        queue_audio::LmsClient::Options cli;
        cli.log = logger;
        cli.notification = [this](const std::string &wire) {
            std::istringstream input(wire);
            std::string id, command, value;
            input >> id >> command >> value;
            command = queue_audio::decodeLms(command);
            value = queue_audio::decodeLms(value);
            if (command != "time" || value.empty() || value == "?")
                return;
            try {
                size_t end = 0;
                double seconds = std::stod(value, &end);
                if (value.front() == '+' || value.front() == '-')
                    seconds += double(mode.status().item.sourceMs) / 1000;
                if (end == value.size() && std::isfinite(seconds) && seconds >= 0 &&
                    seconds * 1000 <= UINT32_MAX) {
                    const auto ms = uint64_t(seconds * 1000);
                    if (ownSeek.load() == ms) {
                        ownSeek = UINT64_MAX;
                        return;
                    }
                    notifiedSeek = ms;
                    logger("queue LMS seek notification source_ms=" + std::to_string(notifiedSeek.load()));
                }
            } catch (...) {
            }
        };
        cli.metrics = [this](uint64_t slow, uint64_t failed) { mode.lmsMetrics(slow, failed); };
        cli.unavailable = [this](const std::string &reason) { post([this, reason] { fallback(reason); }); };
        lms = std::make_unique<queue_audio::LmsClient>(host, mac, std::move(cli));
        queue_audio::ControllerOptions o;
        o.room = room;
        o.session = streamSessionToken();
        o.httpBase = speaker->controllerUri();
        o.managedFallback = true;
        o.log = logger;
        o.lmsControl = [this](const std::string &c) {
            if (c.rfind("time ", 0) == 0)
                ownSeek = uint64_t(std::stod(c.substr(5)) * 1000);
            refetchPending = c.rfind("playlist index ", 0) == 0 || c.rfind("time ", 0) == 0;
            return lms->submit({c}, "sonos-user-action");
        };
        o.lmsPending = [this] { return lms->pending(); };
        o.fallbackRequest = [this](const std::string &reason) {
            if (!roomStop)
                post([this, reason] { fallback(reason); });
        };
        o.reanchor = [this](uint64_t id, uint32_t origin) { queue.reanchorSpeaker(id, origin); };
        o.audiblePosition = [this](uint64_t id, uint64_t frame, yeney::PlaybackState st) {
            post([this, id, frame, st] {
                auto b = bindings.find(id);
                if (b != bindings.end() && mode.accepts(b->second.generation, true) && !refetchPending)
                    queue.speakerPosition(id, frame, st);
            });
        };
        o.confirmed = [this](const queue_audio::ConfirmedItem &e) {
            post([this, e] {
                auto b = bindings.find(e.occurrence);
                if (b == bindings.end() || !mode.accepts(b->second.generation, true))
                    return;
                if (pendingRender == e.occurrence)
                    pendingRender = 0;
                ModeEvidence v;
                v.generation = b->second.generation;
                v.epoch = e.epoch;
                v.occurrence = e.occurrence;
                v.token = b->second.token;
                v.frame = e.frame;
                v.promised = e.promised;
                v.actual = e.actual;
                v.rate = e.rate;
                v.sourceMs = e.sourceMs;
                v.owned = true;
                v.playing = e.state == yeney::PlaybackState::Playing;
                v.ended = e.frame >= e.promised || e.state == yeney::PlaybackState::Stopped;
                for (size_t i = 0; i < playlist.entries.size(); ++i)
                    if (playlist.entries[i].token == v.token)
                        v.index = i;
                v.ahead = e.ahead;
                mode.evidence(v, mono());
            });
        };
        o.prepare = [this](uint64_t token, uint32_t ms) {
            // Called by controller worker; reserve the next core occurrence and
            // let the LMS refetch supply its descriptor/item asynchronously.
            queue_audio::QueueAsset a;
            a.occurrence = queue.latestOccurrence() + 1;
            logger("queue reserve occurrence=" + std::to_string(a.occurrence) +
                   " token=" + std::to_string(token));
            pendingRender = a.occurrence;
            a.token = token;
            a.sourceOriginMs = ms;
            seekOrigin = ms;
            {
                std::lock_guard<std::mutex> lock(preparedMutex);
                prepared[a.occurrence] = {token, mode.status().generation, ms};
            }
            renderPending = true;
            renderAdmission = true;
            refetchPending = false;
            queue.beginRender();
            return a;
        };
        controller = std::make_unique<queue_audio::QueueController>(o, http);
        mode.onRearmLoop([this] { fallback("rearm-loop"); });
        mode.newPlay(true, epoch, 0);
        queueGeneration = mode.status().generation;
        router.bootstrap(queueGeneration);
        router.ready = [this](uint64_t g) {
            coreReady = g;
            changed.notify_one();
        };
        setRoomMode(&mode);
        speaker->queueEvents([this](const upnp::GenaEvent &e) {
            auto v = mode.status();
            post([this, e, v] {
                if (mode.accepts(v.generation, true) && playing)
                    controller->notify(v.item.epoch ? v.item.epoch : epoch.load(), e);
            });
            return true;
        });
        worker = std::thread([this] { work(); });
    }
    void shutdown() {
        speaker->queueEvents({});
        {
            std::lock_guard<std::mutex> l(mutex);
            closing = true;
        }
        changed.notify_all();
        if (lms)
            lms->shutdown();
        if (worker.joinable())
            worker.join();
        if (controller)
            controller->release("service-shutdown").get();
    }
    ~Impl() {
        shutdown();
        setRoomMode(nullptr);
    }
    void post(std::function<void()> f) {
        std::lock_guard<std::mutex> l(mutex);
        if (!closing && jobs.size() < 256) {
            jobs.push_back(std::move(f));
            changed.notify_one();
        }
    }
    void fallback(const std::string &reason, bool brokenCurrent = false) {
        if (mode.status().latched)
            return;
        lms->cancelPending();
        // Faulted current assets cannot promise a remaining tail.
        if (brokenCurrent) {
            auto v = mode.status().item;
            v.usable = false;
            mode.evidence(v, mono());
        }
        mode.request(reason, mono());
        if (mode.status().active == "queue" && playing && !drainRequested) {
            drainRequested = true;
            try {
                controller->drain(epoch).get();
            } catch (const std::exception &e) {
                logger("queue drain-error=" + std::string(e.what()));
            }
        }
    }
    queue_audio::QueueAsset asset(const queue_audio::QueueRecord &r, Binding b) {
        queue_audio::QueueAsset a;
        a.occurrence = r.source.occurrence;
        a.token = b.token;
        a.sourceOriginMs = b.origin;
        a.item = r.item;
        a.strmAt = r.strmAt;
        a.mixedIntro = r.source.transition == 1;
        for (auto &e : playlist.entries)
            if (e.token == b.token) {
                a.metadata.title = e.title;
                a.metadata.artist = e.artist;
                a.metadata.album = e.album;
                a.metadata.artwork = e.artwork(host);
                std::lock_guard<std::mutex> lock(sourceMutex);
                remoteSources[r.source.occurrence] = remoteArtworkTrack(e.id, e.remote, e.url);
                while (remoteSources.size() > 60000)
                    remoteSources.erase(remoteSources.begin());
            }
        return a;
    }
    void sync() {
        const auto state = mode.status();
        if (state.active != "queue")
            return;
        {
            std::lock_guard<std::mutex> lock(preparedMutex);
            bindings.insert(prepared.begin(), prepared.end());
            if (!prepared.empty()) {
                producerToken = prepared.rbegin()->second.token;
                seekOrigin = UINT64_MAX;
                renderPending = false;
            }
            prepared.clear();
        }
        auto records = queue.snapshot();
        if (records.empty() && !playing)
            return;
        bool unbound = false;
        for (auto &r : records)
            if (r.source.occurrence > lastBound && r.source.occurrence < UINT64_MAX / 2)
                unbound = true;
        if (unbound || mono() - lastSnapshot >= 1000) {
            auto fresh = readPlaylist();
            lastSnapshot = mono();
            playlist = std::move(fresh);
            if (playlist.entries.empty()) {
                if (playing)
                    controller->lmsPlaylist(epoch, playlist.revision, 0, {}).get();
                return;
            }
            if (!initialIndex)
                initialIndex = playlist.index;
            for (auto &r : records)
                if (r.incomplete.empty() && r.source.occurrence > lastBound &&
                    r.source.occurrence < UINT64_MAX / 2) {
                    auto reserved = bindings.find(r.source.occurrence);
                    if (reserved != bindings.end()) {
                        lastBound = r.source.occurrence;
                        producerToken = reserved->second.token;
                        continue;
                    }
                    const auto sought = seekOrigin.exchange(UINT64_MAX);
                    const auto origin = sought == UINT64_MAX ? 0 : uint32_t(sought);
                    size_t at = *initialIndex;
                    if (producerToken) {
                        auto previous =
                            std::find_if(playlist.entries.begin(), playlist.entries.end(),
                                         [&](const auto &entry) { return entry.token == producerToken; });
                        if (previous == playlist.entries.end()) {
                            logger("queue producer superseded occurrence=" +
                                   std::to_string(r.source.occurrence) + " reason=playlist-edited");
                            lastBound = r.source.occurrence;
                            continue;
                        }
                        at = std::distance(playlist.entries.begin(), previous) + 1;
                    }
                    if (at >= playlist.entries.size()) {
                        // LMS can start a speculative successor and cancel it
                        // before our snapshot observes the short-lived entry.
                        // It has no authoritative token and must never be
                        // attached to the audible item or trigger a replay.
                        logger("queue producer superseded occurrence=" + std::to_string(r.source.occurrence) +
                               " reason=playlist-edited");
                        lastBound = r.source.occurrence;
                        continue;
                    }
                    bindings[r.source.occurrence] = {playlist.entries[at].token, state.generation, origin};
                    lastBound = r.source.occurrence;
                    producerToken = playlist.entries[at].token;
                }
            for (auto &r : records) {
                auto b = bindings.find(r.source.occurrence);
                if (b != bindings.end() && b->second.generation == state.generation && r.item &&
                    r.incomplete.empty())
                    assets[r.source.occurrence] = asset(r, b->second);
            }
        }

        if (state.latched)
            return;
        std::vector<queue_audio::QueueAsset> plan;
        for (auto &r : records) {
            if (!r.incomplete.empty()) {
                assets.erase(r.source.occurrence);
                continue;
            }
            auto b = bindings.find(r.source.occurrence);
            if (b == bindings.end() || b->second.generation != state.generation || !r.item)
                continue;
            if (!r.fault.empty()) {
                fallback(r.storageFault ? "storage-fault" : "length-fault",
                         r.source.occurrence == state.item.occurrence);
                return;
            }
            auto a = asset(r, b->second);
            assets[a.occurrence] = a;
            plan.push_back(a);
        }
        if (plan.empty())
            return;
        if (pendingRender && state.item.occurrence != pendingRender) {
            controller->prepared(epoch, plan).get();
            return;
        }
        std::vector<queue_audio::QueueAsset> authoritative;
        for (const auto &e : playlist.entries) {
            queue_audio::QueueAsset a;
            a.token = e.token;
            a.occurrence = UINT64_MAX / 2 + e.token;
            for (const auto &v : assets)
                if (v.second.token == e.token)
                    a = v.second;
            authoritative.push_back(std::move(a));
        }
        if (!playing) {
            try {
                if (!plan.front().item->contentLength())
                    return;
            } catch (const queue_audio::PublicationSuperseded &) {
                return; // publication raced this snapshot; refresh on the next tick
            } catch (...) {
                fallback("length-fault");
                return;
            }
            controller->play(epoch, plan.front().occurrence, authoritative).get();
            selectionToken = plan.front().token;
            playing = true;
        } else if (state.item.owned)
            controller->lmsPlaylist(epoch, playlist.revision, state.item.token, authoritative).get();
    }
    void selectSnapshot() {
        const auto state = mode.status();
        if (!playing || state.active != "queue" || state.latched)
            return;
        playlist = readPlaylist();
        if (playlist.entries.empty()) {
            controller->lmsPlaylist(epoch, playlist.revision, 0, {}).get();
            return;
        }
        auto token = playlist.entries[playlist.index].token;
        const auto targetTime = notifiedSeek.exchange(UINT64_MAX);
        const auto previousSelection = selectionToken ? selectionToken : state.item.token;
        selectionToken = token;
        if (token == previousSelection && targetTime != UINT64_MAX)
            playlist.timeMs = uint32_t(targetTime);
        producerToken = token;
        initialIndex = playlist.index;
        queue.restartCached();
        const bool seek =
            token == previousSelection && std::abs(int64_t(playlist.timeMs) - state.item.sourceMs) >= 1000;
        if (seek)
            controller->lmsSeek(epoch, playlist.timeMs).get();
        else {
            std::vector<queue_audio::QueueAsset> plan;
            for (const auto &entry : playlist.entries) {
                queue_audio::QueueAsset a;
                a.token = entry.token;
                a.occurrence = UINT64_MAX / 2 + entry.token;
                for (const auto &cached : assets)
                    if (cached.second.token == entry.token)
                        a = cached.second;
                plan.push_back(std::move(a));
            }
            controller->lmsPlaylist(epoch, playlist.revision, token, plan, true).get();
        }
        if (renderPending.exchange(false)) {
            // Reuse the already received SlimProto start. A CLI jump here
            // restarts LMS and turns track selection into a feedback loop.
            renderAdmission = false;
            manualIntent = false;
            refetchPending = false;
            queue.resumeReceivedStart();
        } else {
            queue.confirmCachedStart();
        }
    }
    struct LmsPending : std::runtime_error {
        LmsPending() : std::runtime_error("lms-pending") {}
    };
    queue_audio::PlaylistSnapshot readPlaylist() {
        const auto at = mono();
        queue_audio::PlaylistSnapshot result;
        std::string reply;
        const bool ok = lms->query("status 0 60000 tags:alKu", reply, [&](const std::string &wire) {
            result = reader.parse(wire);
            return true;
        });
        mode.deferPreparation(mono() - at);
        if (!ok)
            throw LmsPending();
        return result;
    }
    void handoff() {
        auto v = mode.status();
        if (v.active != "stream" || v.state != "handoff" || coreReady != v.generation)
            return;
        unsigned index = v.item.index, ms = v.item.sourceMs;
        if (v.item.ended && v.item.frame >= v.item.promised && v.item.promised &&
            index + 1 < playlist.entries.size()) {
            ++index;
            ms = 0;
        }
        std::vector<std::string> commands = {"playlist index " + std::to_string(index)};
        if (ms)
            commands.push_back("time " + std::to_string(ms / 1000));
        coreReady = 0;
        lms->submit(std::move(commands), "fallback", [this, g = v.generation] {
            post([this, g] {
                if (mode.status().generation != g || mode.status().state != "handoff")
                    return;
                stream.command({'s', 0, false});
                router.admit();
            });
        });
    }
    void work() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(mutex);
                changed.wait_for(l, std::chrono::milliseconds(50), [&] { return closing || !jobs.empty(); });
                if (closing)
                    break;
                if (!jobs.empty()) {
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
            }
            try {
                if (roomStop)
                    continue;
                if (job)
                    job();
                if (queueReadyPending && coreReady == mode.status().generation) {
                    queueReadyPending = false;
                    coreReady = 0;
                    queue.resumeReceivedStart();
                }
                sync();
                mode.tick(mono());
                handoff();
                if (mono() - lastPoll >= 1000) {
                    lastPoll = mono();
                    auto v = mode.status();
                    if (stopped) {
                        try {
                            upnp::QueueCoordinator c(room);
                            c.verifyLease();
                            readiness = true;
                        } catch (...) {
                            readiness = false;
                        }
                    }
                    if (v.active == "stream" && v.state == "handoff") {
                        upnp::TransportInfo info;
                        std::string uri;
                        if (speaker->readTransportInfo(info) && info.state == "PLAYING" &&
                            speaker->currentUri(uri) && uri == streamUri())
                            mode.streamAudible(v.generation, mono(), stream.outputRate());
                    }
                }
            } catch (const LmsPending &) {
                lms->tick(); // No state inferred from a missing/late CLI reply.
            } catch (const std::exception &e) {
                logger("queue room-worker error=" + std::string(e.what()));
                if (!roomStop && !mode.status().latched)
                    fallback(std::string(e.what()).find("playlist-") != std::string::npos ? "lms-unavailable"
                                                                                          : "soap-error");
            }
        }
    }
    void command(const yeney::Command &c) {
        if (c.letter == 'q')
            stopped = true;
        if (c.letter == 's') {
            const auto v = mode.status();
            deferIncoming =
                !router.isQueue() && stopped && v.state == "active" && v.lastFallback != "rearm-loop";
        }
        if (!router.isQueue() && mode.status().state == "active")
            stream.command(c);
        post([this, c] {
            mode.command(c.letter);
            if (mode.status().state == "handoff")
                return; // the one fallback continuation is not a user stop/new play
            if (c.letter == 's')
                mode.beginPreparation(mono());
            if (c.letter == 'q') {
                stopped = true;
                readiness = false;
                mode.stop();
            }
            if (c.letter == 's' && stopped.exchange(false)) {
                if (mode.status().active == "queue") {
                    if (!playing) {
                        producerToken = 0;
                        initialIndex.reset();
                    }
                    mode.adoptPlay();
                    return; // adopt the received start; never request another from LMS
                }
                if (mode.status().lastFallback == "rearm-loop")
                    return; // The circuit breaker requires a service restart.
                try {
                    upnp::QueueCoordinator coordinator(room);
                    coordinator.verifyLease();
                    readiness = true;
                } catch (...) {
                    readiness = false;
                }
                const auto nextEpoch = epoch.load() + 1;
                if (mode.newPlay(readiness, nextEpoch, mono())) {
                    epoch = nextEpoch;
                    auto g = mode.status().generation;
                    queueGeneration = g;
                    stream.flush();
                    router.select(true, g);
                    queueReadyPending = true;
                    router.admit();
                    playing = false;
                    drainRequested = false;
                    assets.clear();
                    bindings.clear();
                    {
                        std::lock_guard<std::mutex> lock(preparedMutex);
                        prepared.clear();
                    }
                    // Reservations may never have reached core. Rearm from
                    // actual strm evidence, not a speculative prepared ID.
                    uint64_t actualLast = 0;
                    for (const auto &record : queue.snapshot())
                        actualLast = std::max(actualLast, record.source.occurrence);
                    lastBound = actualLast;
                    pendingRender = 0;
                    refetchPending = false;
                    manualIntent = false;
                    renderPending = false;
                    renderAdmission = false;
                    seekOrigin = UINT64_MAX;
                    initialIndex.reset();
                    producerToken = selectionToken = 0;
                } else {
                    router.resumeReceivedStart();
                }
            }
        });
    }
};
QueueRoom::QueueRoom(upnp::Speaker r, std::shared_ptr<upnp::StreamServer> h,
                     std::shared_ptr<upnp::OwnSpeakerControl> s, PlaybackSelection p, std::string server,
                     std::string mac, const uint8_t *b, std::function<std::string()> u)
    : impl(std::make_unique<Impl>(std::move(r), std::move(h), std::move(s), std::move(p), std::move(server),
                                  std::move(mac), b, std::move(u))) {}
QueueRoom::~QueueRoom() = default;
void QueueRoom::shutdown() { impl->shutdown(); }
yeney::Sink &QueueRoom::sink() { return impl->router; }
void QueueRoom::command(const yeney::Command &c) { impl->command(c); }
bool QueueRoom::queueActive() const { return impl->mode.status().active == "queue"; }
bool QueueRoom::deferStart() { return impl->deferIncoming.exchange(false); }
bool QueueRoom::remoteSource(uint64_t id) const {
    std::lock_guard<std::mutex> lock(impl->sourceMutex);
    auto found = impl->remoteSources.find(id);
    return found == impl->remoteSources.end() || found->second;
}
} // namespace bridge

void bridge::runQueueCoreClient(const char *server, const uint8_t *mac, const char *name, QueueRoom &room) {
    for (int sig : {SIGINT, SIGTERM, SIGQUIT, SIGHUP})
        std::signal(sig, bridge::stopRoom);
    try {
        yeney::Config config;
        config.server = server ? server : "";
        auto colon = config.server.find(':');
        if (colon != std::string::npos) {
            auto port = std::stoul(config.server.substr(colon + 1));
            if (!port || port > 65535 || port == 9000)
                throw std::runtime_error("invalid Slimproto port");
            config.port = port;
            config.server.resize(colon);
        }
        config.name = name;
        std::copy(mac, mac + 6, config.mac.begin());
        config.startOnSubmit = true;
        config.queueNextDelayMs = bridge::queueNextDelayMs();
        config.queueRelativeClock = true;
        config.deferQueueStart = [&room] { return room.deferStart(); };
        config.queueRemoteSource = [&room](uint64_t id) { return room.remoteSource(id); };
        config.log = [](const std::string &s) { printf("core: %s\n", s.c_str()); };
        config.observeCommand = [&room](const yeney::Command &c) { room.command(c); };
        yeney::Player player(config, room.sink());
        player.run(bridge::roomStop);
    } catch (const std::exception &e) {
        printf("core: fatal: %s\n", e.what());
        std::exit(1);
    }
    room.shutdown();
    std::exit(0);
}
