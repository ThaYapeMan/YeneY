// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_route.h"
#include "upnp/queue_metadata.h"
#include "upnp/speaker_control.h"
#include <functional>
#include <future>
namespace queue_audio {
struct QueueAsset {
    uint64_t occurrence = 0, revision = 1;
    std::shared_ptr<Item> item; // may be null until the decoder creates it
    upnp::QueueMetadata metadata;
};
struct ControllerOptions {
    upnp::Speaker room;
    std::string session, httpBase;
    std::function<void(const std::string &)> log;
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
// Shared immutable items permit independent HTTP readers. No audible callbacks
// or HTTP-derived timing are implemented here (phase 3b part 3).
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
    std::future<void> release(std::string reason = "stop");
    ControllerStatus status() const;
};
} // namespace queue_audio
