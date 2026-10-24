extern "C" void yeney_stream_track_changed() __attribute__((weak));
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
namespace queue_room_detail {
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
} // namespace queue_room_detail
QueueRoom::Impl::Impl(upnp::Speaker r, std::shared_ptr<upnp::StreamServer> http,
                      std::shared_ptr<upnp::OwnSpeakerControl> s, std::string h, std::string m,
                      const uint8_t *rawMac, std::function<std::string()> url)
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
QueueRoom::Impl::~Impl() { shutdown(); }
int64_t QueueRoom::Impl::milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void QueueRoom::Impl::shutdown() {
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

QueueRoom::QueueRoom(upnp::Speaker room, std::shared_ptr<upnp::StreamServer> http,
                     std::shared_ptr<upnp::OwnSpeakerControl> speaker, PlaybackSelection, std::string host,
                     std::string mac, const uint8_t *rawMac, std::function<std::string()> url)
    : impl(new Impl(std::move(room), std::move(http), std::move(speaker), std::move(host), std::move(mac),
                    rawMac, std::move(url))) {}
QueueRoom::~QueueRoom() = default;
yeney::Sink &QueueRoom::sink() { return impl->paths; }
void QueueRoom::shutdown() { impl->shutdown(); }
void QueueRoom::command(const yeney::Command &c) { impl->received(c); }
void QueueRoom::refusePlayback(bool member) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->commands.push_back({member ? 'M' : 'R', 0});
    impl->changed.notify_all();
}
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
    config.queueHostFailures = true;
    config.queueRelativeClock = true;
    config.queueRemoteSource = [&room](uint64_t id) { return room.remoteSource(id); };
    config.log = [](const auto &line) { std::printf("core: %s\n", line.c_str()); if ((line.rfind("STMs ", 0) == 0 || line.rfind("STMt ", 0) == 0) && yeney_stream_track_changed) yeney_stream_track_changed(); };
    yeney::Player player(config, room.sink());
    player.run(roomStop);
    room.shutdown();
    icon.shutdown();
    std::exit(0);
}
} // namespace bridge
