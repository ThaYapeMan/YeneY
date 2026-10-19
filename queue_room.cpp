// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room.h"
#include "core_queue_sink.h"
#include "core_sonos.h"
#include "mode_sink.h"
#include "queue_controller.h"
#include "queue_playlist.h"
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
    std::unique_ptr<queue_audio::QueueController> controller;
    queue_audio::PlaylistReader reader;
    queue_audio::PlaylistSnapshot playlist;
    struct Binding {
        uint64_t token, generation;
        uint32_t origin;
    };
    std::map<uint64_t, Binding> bindings;
    std::map<uint64_t, queue_audio::QueueAsset> assets;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> jobs;
    std::thread worker;
    bool closing = false, playing = false, drainRequested = false, queueReadyPending = false;
    std::atomic<uint64_t> epoch{1}, lastBound{0};
    uint64_t lastSnapshot = 0, lastPoll = 0;
    std::optional<unsigned> initialIndex;
    uint64_t producerToken = 0;
    std::atomic<uint64_t> coreReady{0};
    std::atomic<bool> stopped{false}, readiness{true};
    std::atomic<uint64_t> queueGeneration{1};
    std::atomic<uint64_t> seekOrigin{UINT64_MAX};
    std::atomic<bool> refetchPending{false}, manualIntent{false}, renderPending{false};
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
            if (c == 'f')
                manualIntent = !refetchPending;
            if (c == 'p' || c == 'u' || c == 'q' || c == 'f') {
                post([this, c] {
                    if (playing)
                        controller->lmsCommand(epoch, c).get();
                });
                return true;
            }
            if (c == 's' && refetchPending.exchange(false)) {
                manualIntent = false;
                post([this] {
                    if (playing)
                        controller->lmsCommand(epoch, 's');
                });
                return true;
            }
            if (c == 's' && manualIntent.exchange(false)) {
                post([this] { selectSnapshot(); });
                return true;
            }
            return false;
        });
        queue_audio::ControllerOptions o;
        o.room = room;
        o.session = streamSessionToken();
        o.httpBase = speaker->controllerUri();
        o.managedFallback = true;
        o.log = logger;
        o.lmsControl = [this](const std::string &c) {
            if (c.rfind("time ", 0) == 0)
                seekOrigin = uint32_t(std::stod(c.substr(5)) * 1000);
            refetchPending = c.rfind("playlist index ", 0) == 0 || c.rfind("time ", 0) == 0;
            return queue_audio::controlLms(host, mac, c);
        };
        o.fallbackRequest = [this](const std::string &reason) { post([this, reason] { fallback(reason); }); };
        o.reanchor = [this](uint64_t id, uint32_t origin) { queue.reanchorSpeaker(id, origin); };
        o.audiblePosition = [this](uint64_t id, uint64_t frame, yeney::PlaybackState st) {
            post([this, id, frame, st] {
                auto b = bindings.find(id);
                if (b != bindings.end() && mode.accepts(b->second.generation, true))
                    queue.speakerPosition(id, frame, st);
            });
        };
        o.confirmed = [this](const queue_audio::ConfirmedItem &e) {
            post([this, e] {
                auto b = bindings.find(e.occurrence);
                if (b == bindings.end() || !mode.accepts(b->second.generation, true))
                    return;
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
            a.occurrence = lastBound.fetch_add(1) + 1;
            a.token = token;
            a.sourceOriginMs = ms;
            seekOrigin = ms;
            {
                std::lock_guard<std::mutex> lock(preparedMutex);
                prepared[a.occurrence] = {token, mode.status().generation, ms};
            }
            renderPending = true;
            refetchPending = false;
            queue.beginRender();
            return a;
        };
        controller = std::make_unique<queue_audio::QueueController>(o, http);
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
            auto fresh = reader.read(host, mac);
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
                if (r.source.occurrence > lastBound && r.source.occurrence < UINT64_MAX / 2) {
                    const auto sought = seekOrigin.exchange(UINT64_MAX);
                    const auto origin = sought == UINT64_MAX ? 0 : uint32_t(sought);
                    size_t at = *initialIndex;
                    if (producerToken) {
                        auto previous =
                            std::find_if(playlist.entries.begin(), playlist.entries.end(),
                                         [&](const auto &entry) { return entry.token == producerToken; });
                        if (previous == playlist.entries.end())
                            throw std::runtime_error("playlist-producer-token-missing");
                        at = std::distance(playlist.entries.begin(), previous) + 1;
                    }
                    if (at >= playlist.entries.size())
                        throw std::runtime_error("playlist-occurrence-unmapped");
                    bindings[r.source.occurrence] = {playlist.entries[at].token, state.generation, origin};
                    lastBound = r.source.occurrence;
                    producerToken = playlist.entries[at].token;
                }
            for (auto &r : records) {
                auto b = bindings.find(r.source.occurrence);
                if (b != bindings.end() && b->second.generation == state.generation && r.item)
                    assets[r.source.occurrence] = asset(r, b->second);
            }
        }

        if (state.latched)
            return;
        std::vector<queue_audio::QueueAsset> plan;
        for (auto &r : records) {
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
            } catch (...) {
                fallback("length-fault");
                return;
            }
            controller->play(epoch, plan.front().occurrence, authoritative).get();
            playing = true;
        } else if (state.item.owned)
            controller->lmsPlaylist(epoch, playlist.revision, state.item.token, authoritative).get();
    }
    void selectSnapshot() {
        const auto state = mode.status();
        if (!playing || state.active != "queue" || state.latched)
            return;
        playlist = reader.read(host, mac);
        if (playlist.entries.empty()) {
            controller->lmsPlaylist(epoch, playlist.revision, 0, {}).get();
            return;
        }
        auto token = playlist.entries[playlist.index].token;
        const bool seek =
            token == state.item.token && std::abs(int64_t(playlist.timeMs) - state.item.sourceMs) >= 1000;
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
            if (!queue_audio::controlLms(host, mac, "playlist index " + std::to_string(playlist.index)))
                throw std::runtime_error("playlist-render-unconfirmed");
            if (seek && !queue_audio::controlLms(host, mac, "time " + std::to_string(playlist.timeMs / 1000)))
                throw std::runtime_error("playlist-seek-unconfirmed");
        }
    }
    void handoff() {
        auto v = mode.status();
        if (v.active != "stream" || v.state != "handoff" || coreReady != v.generation)
            return;
        coreReady = 0;
        unsigned index = v.item.index, ms = v.item.sourceMs;
        if (v.item.ended && v.item.frame >= v.item.promised && v.item.promised &&
            index + 1 < playlist.entries.size()) {
            ++index;
            ms = 0;
        }
        if (!queue_audio::controlLms(host, mac, "playlist index " + std::to_string(index)))
            throw std::runtime_error("handoff-cli-index");
        if (ms && !queue_audio::controlLms(host, mac, "time " + std::to_string(ms / 1000)))
            throw std::runtime_error("handoff-cli-time");
        stream.command({'s', 0, false});
        router.admit();
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
                if (job)
                    job();
                if (queueReadyPending && coreReady == mode.status().generation) {
                    queueReadyPending = false;
                    coreReady = 0;
                    playlist = reader.read(host, mac);
                    if (!queue_audio::controlLms(host, mac,
                                                 "playlist index " + std::to_string(playlist.index)))
                        throw std::runtime_error("playlist-rearm-unconfirmed");
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
            } catch (const std::exception &e) {
                logger("queue room-worker error=" + std::string(e.what()));
                if (!mode.status().latched)
                    fallback(std::string(e.what()).find("playlist-cli") != std::string::npos
                                 ? "preparation-timeout"
                             : std::string(e.what()).find("playlist-") != std::string::npos
                                 ? "foreign-control"
                                 : "soap-error");
            }
        }
    }
    void command(const yeney::Command &c) {
        if (!router.isQueue() && mode.status().state == "active")
            stream.command(c);
        post([this, c] {
            if (c.letter == 's')
                mode.beginPreparation(mono());
            if (c.letter == 'q') {
                stopped = true;
                readiness = false;
                mode.stop();
            }
            if (c.letter == 's' && stopped.exchange(false)) {
                try {
                    upnp::QueueCoordinator coordinator(room);
                    coordinator.verifyLease();
                    readiness = true;
                } catch (...) {
                    readiness = false;
                }
                if (mode.newPlay(readiness, ++epoch, mono())) {
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
                    refetchPending = false;
                    manualIntent = false;
                    renderPending = false;
                    seekOrigin = UINT64_MAX;
                    initialIndex.reset();
                    producerToken = 0;
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
