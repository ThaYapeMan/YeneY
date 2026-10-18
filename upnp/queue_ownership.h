// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace upnp {
struct QueueSnapshot {
    uint32_t update = 0;
    std::vector<std::string> uris;
};
struct SavedQueue {
    std::string id, title;
    uint64_t created = 0;
};
struct QueueOwnershipIO {
    std::function<QueueSnapshot()> browse;
    std::function<std::vector<SavedQueue>()> playlists;
    std::function<std::string(const std::string &)> save;
    std::function<void(const std::string &)> deletePlaylist;
    std::function<bool()> crossfade;
    std::function<void(bool)> setCrossfade;
    std::function<void(uint32_t, unsigned, unsigned)> remove;
    std::function<void(const std::string &)> log;
};
// The serialized room controller supplies coordinator-fenced I/O. Exceptions
// stop the mutation plan; they must trigger its latched fallback, never retries
// against an old ordinal snapshot.
class QueueOwnership {
    QueueOwnershipIO io;
    std::string session;
    bool held = false, restoring = false, savedCrossfade = false;
    bool owns(const std::string &) const;

public:
    QueueOwnership(QueueOwnershipIO, std::string session);
    void acquire(const std::string &backupTitle);
    void release();
    bool active() const { return held; }
};
} // namespace upnp
