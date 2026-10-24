// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::fastPoll() {
    fastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    nextPoll = {};
}
upnp::QueueOwnershipIO QueueRoom::Impl::ownershipIO() {
    upnp::QueueOwnershipIO io;
    io.browse = [this] { return coordinator.browse(); };
    io.owned = [this](const auto &uri) { return owned.count(uri) != 0; };
    io.playlists = [this] { return coordinator.playlists(); };
    io.save = [this](const auto &title) { return actions.save(title).response.value("AssignedObjectID"); };
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
void QueueRoom::Impl::release() {
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
void QueueRoom::Impl::command(char c, uint64_t next) {
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
void QueueRoom::Impl::append() {
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
            logQueue("metadata sanitized mode=queue occurrence=" + std::to_string(record.source.occurrence) +
                     " " + changes);
        owned.insert(metadata.url); // include an enqueue whose reply is lost
        const auto result = actions.add(metadata.url, upnp::queueDidl(metadata));
        expected.push_back(metadata.url);
        const auto after = coordinator.browse();
        if (result.response.value("NumTracksAdded") != "1" || after.uris != expected ||
            after.update == before.update)
            throw std::runtime_error("queue-enqueue-unconfirmed");
        items.push_back({record.source.occurrence, metadata.url, record.item, track.url, metadata.frames});
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
void QueueRoom::Impl::run() {
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
} // namespace bridge
