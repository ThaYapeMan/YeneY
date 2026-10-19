// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_route.h"
#include "upnp/gena.h"
#include "upnp/queue_metadata.h"
#include "upnp/speaker_control.h"
#include <functional>
#include <future>
namespace queue_audio {
struct QueueAsset {
    uint64_t occurrence = 0, revision = 1;
    uint64_t token = 0; // stable LMS playlist occurrence; defaults to occurrence
    uint32_t sourceOriginMs = 0;
    bool mixedIntro = false;
    std::shared_ptr<Item> item; // may be null until the decoder creates it
    upnp::QueueMetadata metadata;
    std::chrono::steady_clock::time_point strmAt{};
};
struct ControllerOptions {
    upnp::Speaker room;
    std::string session, httpBase;
    std::function<void(const std::string &)> log;
    // Receives only confirmed speaker identity/position, on the controller
    // worker. Bind to CoreQueueSink::speakerPosition before the player is
    // started.
    std::function<void(uint64_t, uint64_t, yeney::PlaybackState)> audiblePosition;
    // Nonblocking host preparation: return a fresh occurrence/URL for a missing
    // asset or seek; item may be pending and supplied by a later LMS snapshot.
    std::function<QueueAsset(uint64_t, uint32_t)> prepare;
    std::function<void(uint64_t, uint32_t)> reanchor;
    // Part 5 host boundary. A fallbackRequest enables app reconciliation.
    // Bind both before use; execution stays on the worker.
    // lmsControl returns only after a decoded CLI acknowledgement; never retry.
    std::function<bool(const std::string &)> lmsControl;
    std::function<void(const std::string &)> fallbackRequest;
    double deviceRatePrior = 1;               // qualified device rate; phase is never carried
    std::function<std::string()> backupTitle; // default local time, minute precision
};
struct ControllerStatus {
    bool active = false;
    uint64_t epoch = 0;
    size_t mirrored = 0;
    std::string reason = "idle", fault;
};
// One serialized worker per room. The host forwards LMS intent and ordered
// current/future occurrence descriptors; it never waits inside a core callback.
// Shared immutable items permit independent HTTP readers. Only speaker evidence
// advances the audible clock; HTTP progress never enters this controller.
class QueueController {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    QueueController(ControllerOptions, std::shared_ptr<upnp::StreamServer>);
    ~QueueController();
    QueueController(const QueueController &) = delete;
    QueueController &operator=(const QueueController &) = delete;
    // Snapshot includes current plus at least the next three occurrences when
    // the playlist has them. Unpublished entries remain pending in queue order.
    std::future<void> play(uint64_t epoch, uint64_t current, std::vector<QueueAsset>);
    std::future<void> update(uint64_t epoch, std::vector<QueueAsset>);
    // Authoritative LMS playlist snapshot, including stable duplicate tokens.
    // A higher revision edits future entries only. select is explicit user
    // intent, unlike decoder read-ahead s. current is a token, never a Sonos
    // ordinal.
    std::future<void> lmsPlaylist(uint64_t epoch, uint64_t revision, uint64_t current,
                                  std::vector<QueueAsset>, bool select = false);
    std::future<void> lmsCommand(uint64_t epoch, char command);
    std::future<void> lmsSeek(uint64_t epoch, uint32_t sourceMs);
    std::future<void> release(std::string reason = "stop");
    // Forward only SID/coordinator-validated AVTransport notifications from the
    // room's existing subscription owner. Capture epoch when receiving, not
    // later.
    std::future<void> notify(uint64_t epoch, upnp::GenaEvent);
    ControllerStatus status() const;
};
} // namespace queue_audio
