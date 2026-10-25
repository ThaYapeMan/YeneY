// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_ownership.h"
#include <algorithm>
#include <exception>
#include <stdexcept>
namespace upnp {
QueueOwnership::QueueOwnership(QueueOwnershipIO callbacks, std::string token)
    : io(std::move(callbacks)), session(std::move(token)) {
    if (session.empty() ||
        session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
        throw std::invalid_argument("invalid queue ownership session");
}
bool QueueOwnership::owns(const std::string &uri) const {
    if (io.owned)
        return io.owned(uri);
    const auto path = uri.find("/music/queue.wav?");
    if (path == std::string::npos)
        return false;
    auto at = path + 17;
    while (at < uri.size()) {
        const auto end = uri.find('&', at);
        if (uri.substr(at, end == std::string::npos ? end : end - at) == "session=" + session)
            return true;
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return false;
}
void QueueOwnership::acquire(const std::string &title) {
    if (held || restoring)
        throw std::logic_error("queue ownership already held");
    auto queue = io.browse();
    const bool foreign =
        std::any_of(queue.uris.begin(), queue.uris.end(), [&](const auto &u) { return !owns(u); });
    if (foreign) {
        if (title.compare(0, 13, "Before YeneY ") != 0)
            throw std::invalid_argument("invalid queue backup prefix");
        auto id = io.save(title);
        auto saved = io.playlists();
        auto verified = std::find_if(saved.begin(), saved.end(),
                                     [&](const auto &p) { return p.id == id && p.title == title; });
        if (id.empty() || verified == saved.end())
            throw std::runtime_error("queue-backup-unverified");
        io.log("queue backup id=" + id + " title=" + title);
        // Verify identity, not just same-minute title. Sort ties by ID to make
        // retention deterministic without deleting the newly verified backup.
        saved.erase(
            std::remove_if(saved.begin(), saved.end(),
                           [](const auto &p) { return p.title.compare(0, 13, "Before YeneY ") != 0; }),
            saved.end());
        std::stable_sort(saved.begin(), saved.end(), [&](const auto &a, const auto &b) {
            if (a.id == id)
                return b.id != id;
            if (b.id == id)
                return false;
            if (a.created != b.created)
                return a.created > b.created;
            auto sequence = [](const std::string &id) {
                const auto colon = id.rfind(':');
                const auto number = id.substr(colon == std::string::npos ? 0 : colon + 1);
                if (number.empty() || number.size() > 19 ||
                    number.find_first_not_of("0123456789") != std::string::npos)
                    return uint64_t(0);
                return uint64_t(std::stoull(number));
            };
            auto left = sequence(a.id), right = sequence(b.id);
            return left != right ? left > right : a.id > b.id;
        });
        for (size_t i = 5; i < saved.size(); ++i) {
            const auto fresh = io.playlists();
            const auto &victim = saved[i];
            auto verified = std::find_if(fresh.begin(), fresh.end(), [&](const auto &p) {
                return p.id == victim.id && p.title == victim.title && p.created == victim.created;
            });
            if (verified == fresh.end())
                throw std::runtime_error("queue-backup-identity-changed");
            io.deletePlaylist(victim.id);
        }
        auto fresh = io.browse();
        if (fresh.update != queue.update || fresh.uris != queue.uris)
            throw std::runtime_error("queue-changed-during-backup");
    }
    savedCrossfade = io.crossfade();
    restoring = true;
    io.setCrossfade(false);
    // A single fenced removal preserves the verified backup. The callback must
    // reject stale UpdateID, and must never substitute RemoveAll on conflict.
    if (!queue.uris.empty())
        io.remove(queue.update, 1, queue.uris.size());
    held = true;
}
void QueueOwnership::release(bool cleanup, std::function<bool(const std::string &)> cleanupOwns, bool stop) noexcept {
    const bool removeOwned = held, restore = restoring, crossfade = savedCrossfade;
    // Local ownership ends before any fallible speaker I/O, including partial acquisition.
    held = restoring = savedCrossfade = false;
    if (!cleanup) return;
    try {
        if (stop && io.stop) io.stop();
        if (removeOwned) {
            auto queue = io.browse();
            auto remaining = queue.uris.size();
            const auto owned = [&](const auto &uri) { return cleanupOwns ? cleanupOwns(uri) : owns(uri); };
            for (;;) {
                size_t last = queue.uris.size();
                while (last && !owned(queue.uris[last - 1])) --last;
                if (!last) break;
                size_t first = last - 1;
                while (first && owned(queue.uris[first - 1])) --first;
                if (!remaining--) throw std::runtime_error("queue-release-did-not-converge");
                io.remove(queue.update, first + 1, last - first);
                queue = io.browse();
            }
        }
        if (restore) io.setCrossfade(crossfade);
    } catch (const std::exception &e) {
        // A refusal also terminates the cleanup plan: never try another write.
        const std::string error = e.what();
        const bool refusal = error.find("fault=7") != std::string::npos;
        try { io.log(std::string("cleanup ") + (refusal ? "transport no longer ours: " : "failed: ") + error); }
        catch (...) {}
    } catch (...) {
        try { io.log("cleanup failed: unknown error"); } catch (...) {}
    }
}
} // namespace upnp
