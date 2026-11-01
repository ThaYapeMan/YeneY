// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::reconcileWindow() {
    if (!started || faulted || externalReleased || sourceAway || !current || selecting) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < nextWindowPoll) return;
    nextWindowPoll = now + std::chrono::milliseconds(pollMs);
    queue_audio::PlaylistSnapshot snapshot;
    try {
        snapshot = reader.read(host, mac);
    } catch (const std::exception &) {
        // A failed read supplies no evidence of an LMS edit. Keep the last
        // confirmed window; the next ordinary worker poll may try a new read.
        logQueue("window snapshot unavailable reason=cli-read");
        return;
    }
    const auto previous = publishedWindowPolicy;
    window.reconcile(snapshot);
    const bool changed = windowDirty || previous.revision != snapshot.revision || previous.shuffle != snapshot.shuffle ||
                         previous.repeat != snapshot.repeat;
    if (!changed) return;
    using Edits = queue_audio::WindowEdits;
    std::vector<Edits::Parent> before, wanted;
    std::map<uint64_t, std::vector<Entry>> entries;
    uint64_t anchor = 0;
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        for (const auto &entry : items) {
            const auto owner = entry.owner ? entry.owner : entry.stream;
            const auto token = sourceTokens.find(owner);
            if (token == sourceTokens.end()) return; // No speculative identity matching.
            if (before.empty() || before.back().token != token->second)
                before.push_back({token->second, {}});
            before.back().uris.push_back(entry.uri);
            entries[token->second].push_back(entry);
            if (entry.stream == current) anchor = token->second;
            if (const auto slot = window.find(token->second)) sourceIndices[owner] = slot->index;
        }
    }
    const auto audible = window.find(anchor);
    if (!audible) {
        // Deleting/clearing the audible occurrence makes preserving its LMS
        // identity impossible. Release only our owned queue; no CLI echo.
        logQueue("window replace reason=audible-anchor-gone lms_commands=0");
        release(true);
        queue.flush();
        firstStream = queue.latestOccurrence() + 1;
        stoppedPlay = true;
        playlist = std::move(snapshot);
        publishedWindowPolicy = window.policy();
        windowDirty = false;
        return;
    }
    for (const auto &slot : window.around(audible->index)) {
        const auto found = entries.find(slot.token);
        if (found == entries.end()) {
            if (slot.index <= audible->index) continue; // History selection is a later stage.
            const auto lazy = std::make_shared<queue_audio::LazyItem>(nullptr, [this, token = slot.token] {
                std::lock_guard<std::mutex> lock(mutex);
                if (closing) return;
                commands.push_back({'L', token});
                this->changed.notify_all();
            }, true);
            upnp::QueueMetadata metadata;
            metadata.url = speaker->controllerUri() + route.publishDeferred(1, "lazy-" + std::to_string(slot.token) + "-" + std::to_string(++lazySerial),
                [lazy, token = slot.token](upnp::StreamRequest &request) {
                    const auto start = std::chrono::steady_clock::now();
                    auto item = lazy->resolve(request, std::chrono::seconds(3));
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
                    std::printf("yeney: queue lazy %s token=%llu elapsed_ms=%lld lms_commands=%u\n",
                        item ? "headers" : (ms >= 2990 ? "deadline" : "abandoned"),
                        (unsigned long long)token, (long long)ms, lazy->commandCount());
                    return item;
                }, [lazy] { return lazy->ready(); });
            metadata.session = session;
            metadata.title = slot.track.title; metadata.artist = slot.track.artist; metadata.album = slot.track.album;
            metadata.artwork = slot.track.artwork(host);
            metadata.rate = 1; // Duration scale only; deferred DIDL omits sampleFrequency.
            metadata.frames = uint64_t(slot.track.duration);
            metadata.deferred = true;
            upnp::sanitizeXmlFields({{"title", &metadata.title}, {"artist", &metadata.artist},
                {"album", &metadata.album}, {"artwork", &metadata.artwork}});
            // Placeholder identities descend from the top; core continuations grow
            // upward from the midpoint. Never alias their first continuation IDs.
            const auto id = UINT64_MAX - slot.token;
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                sourceTokens[id] = slot.token;
                sourceIndices[id] = slot.index;
            }
            lazyItems[slot.token] = lazy;
            entries[slot.token].push_back({id, metadata.url, {}, slot.track.url, 0, metadata.title, 0, 0, upnp::queueDidl(metadata)});
            owned.insert(metadata.url);
            size_t ordinal = 1;
            for (const auto &parent : wanted) ordinal += parent.uris.size();
            logQueue("lazy declared ordinal=" + std::to_string(ordinal) + " lms_index=" + std::to_string(slot.index) +
                " kind=" + (remotePlaybackSource(slot.track.id, slot.track.remote, slot.track.url) ? "upper-bound" : "deferred"));
        }
        Edits::Parent parent{slot.token, {}};
        for (const auto &entry : entries.at(slot.token)) parent.uris.push_back(entry.uri);
        wanted.push_back(std::move(parent));
    }
    // Live/unknown duration is outside the finite window model. Preserve the
    // established natural producer path rather than discard its audible URI.
    if (wanted.empty()) return;
    Edits::IO io;
    io.browse = [this] {
        const auto value = coordinator.browse();
        return Edits::Snapshot{value.update, value.uris};
    };
    io.remove = [this](auto update, auto first, auto count) { actions.remove(update, first, count); };
    io.insert = [this, &entries](auto token, auto segment, auto ordinal) {
        const auto &entry = entries.at(token).at(segment);
        const auto reply = actions.add(entry.uri, entry.didl, ordinal);
        if (reply.response.value("NumTracksAdded") != "1")
            throw std::runtime_error("queue-window-enqueue-unconfirmed");
    };
    io.log = [&snapshot, &before, this](const auto &message) {
        std::string indices;
        for (const auto &parent : before) {
            const auto slot = window.find(parent.token);
            indices += (indices.empty() ? "" : ",") + (slot ? std::to_string(slot->index) : "removed");
        }
        logQueue(message + " lms_indices=" + indices + " reason=playlist-change" +
                 " shuffle=" + std::to_string(snapshot.shuffle) + " repeat=" + std::to_string(snapshot.repeat));
    };
    Edits(std::move(io)).apply(before, wanted, anchor);
    std::vector<Entry> reordered;
    std::set<uint64_t> retained;
    for (const auto &parent : wanted) {
        retained.insert(parent.token);
        for (const auto &entry : entries.at(parent.token)) reordered.push_back(entry);
    }
    for (const auto &parent : before) {
        if (retained.count(parent.token)) continue;
        const auto &entry = entries.at(parent.token).front();
        if (const auto lazy = lazyItems.find(parent.token); lazy != lazyItems.end()) lazy->second->cancel();
        queue.releaseParent(entry.owner ? entry.owner : entry.stream);
        for (const auto &uri : parent.uris) owned.erase(uri);
    }
    items.swap(reordered);
    playlist = std::move(snapshot);
    publishedWindowPolicy = window.policy();
    windowDirty = false;
    publish();
}
void QueueRoom::Impl::activateLazy(uint64_t token) {
    // A naturally arriving stream owns its declaration even while count/rate
    // discovery is pending. Its HTTP reader waits, without selecting it again.
    append();
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        for (const auto &record : queue.snapshot()) {
            const auto identity = sourceTokens.find(record.source.occurrence);
            if (!record.parent && identity != sourceTokens.end() && identity->second == token &&
                record.incomplete.empty() && record.fault.empty()) return;
        }
    }
    auto found = lazyItems.find(token);
    const auto slot = window.find(token);
    if (found == lazyItems.end() || !slot || activatingToken || !found->second->claimActivation()) return;
    activatingToken = token;
    lazyTarget = int(slot->index);
    try {
        roomLmsCommand(host, mac, "playlist index " + std::to_string(slot->index), "lazy-pcm-get", logQueue);
        logQueue("lazy activated lms_index=" + std::to_string(slot->index) + " lms_commands=1");
    } catch (...) {
        found->second->cancel(); lazyTarget = -1; activatingToken = 0;
        logQueue("lazy abandoned reason=activation-failed lms_commands=1");
    }
}
} // namespace bridge
