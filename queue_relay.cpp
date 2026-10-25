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
    io.stop = [this] { actions.stop(); };
    io.log = logQueue;
    return io;
}
void QueueRoom::Impl::release(bool stop) {
    auto cleanupOwned = std::move(owned);
    owned.clear();
    items.clear();
    releasing = true;
    ownership.release(!sourceAway,
                      [cleanupOwned = std::move(cleanupOwned)](const auto &uri) { return cleanupOwned.count(uri) != 0; }, stop);
    releasing = false;
    started = false;
    waitingForPlay = false;
    current = 0;
    lastElapsed = 0;
    state = "STOPPED";
    publish();
}
void QueueRoom::Impl::command(char c, uint64_t next) {
    if (c == 'M' || c == 'R') {
        if (!roomStopped) {
            if (c == 'M') {
                const auto live = speaker->speaker();
                std::printf("yeney: room is group member room=%s coordinator=%s; not playing\n",
                            room.name.c_str(), live.coordinator.c_str());
            }
            roomFault(c == 'M' ? "group-member" : "topology-unavailable", c == 'M');
        }
        return;
    }
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
        if (!roomStopped) release(ownership.active() && !externalReleased);
        faulted = false;
        fault.clear();
        playlist = {};
        publish();
    } else if (c == 's' && externalReleased) {
        externalReleased = false;
        sourceAway = false;
        faulted = false;
        fault.clear();
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
            { if (record.storageFault) throw std::runtime_error(record.fault); throw TrackFault(record.fault); }
        if (!record.item || !record.incomplete.empty())
            continue;
        if (std::any_of(items.begin(), items.end(),
                        [&](const auto &i) { return i.stream == record.source.occurrence; }))
            continue;
        if (record.source.rate > queue.maxSampleRate())
            throw TrackFault("queue-rate-unsupported rate=" + std::to_string(record.source.rate) +
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
            { if (record.storageFault) throw std::runtime_error(record.fault); throw TrackFault(record.fault); }
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
        logQueue("item occurrence=" + std::to_string(record.source.occurrence) +
                 " title=" + RoomStatus::quote(metadata.title));
        expected.push_back(metadata.url);
        const auto after = coordinator.browse();
        if (result.response.value("NumTracksAdded") != "1" || after.uris != expected ||
            after.update == before.update)
            throw std::runtime_error("queue-enqueue-unconfirmed");
        items.push_back({record.source.occurrence, metadata.url, record.item, track.url, metadata.frames, metadata.title});
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
void QueueRoom::Impl::roomFault(const std::string &reason, bool member) {
    if (roomStopped) return;
    roomStopped = true;
    fault = reason;
    faulted = true;
    selecting = selectionKnown = false;
    paths.cancelSelection();
    if (!member) {
        try { release(!externalReleased); }
        catch (const std::exception &e) { logQueue("release failed=" + std::string(e.what())); }
    }
    if (member) {
        externalReleased = true;
        sourceAway = true;
        release();
    }
    // A member must never write to either its old queue or the new coordinator.
    expectedState = state = "STOPPED";
    started = waitingForPlay = false;
    externalReleased = true;
    stopAcknowledged = false;
    publish();
    speakerCommand("stop", member ? "group-member" : reason == "track-fault-limit" ? reason : "room-fault");
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
                    if (externalReleased) stopAcknowledged = true;
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
            if (sourceAway && started && !externalReleased) {
                externalStop("speaker-source-change");
                continue;
            }
            if (roomStopped) continue;
            fault = e.what();
            logQueue("relay fault=" + fault);
            const bool trackFault = dynamic_cast<const TrackFault *>(&e) != nullptr;
            if (!trackFault) {
                roomFault(fault, fault.compare(0, 17, "room-group-member") == 0);
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            while (!trackFaults.empty() && now - trackFaults.front() > std::chrono::seconds(10))
                trackFaults.pop_front();
            trackFaults.push_back(now);
            if (trackFaults.size() >= 2) {
                logQueue("track-fault-limit: two track faults within 10 s; stopping room");
                roomFault("track-fault-limit");
                continue;
            }
            faulted = true;
            try { release(!externalReleased); }
            catch (const std::exception &cleanup) { logQueue(cleanup.what()); }
            queue.failQueueTrack();
            publish();
        }
    }
    if (roomStopped) return;
    try {
        release(ownership.active() && !externalReleased);
    } catch (const std::exception &e) {
        logQueue("release failed=" + std::string(e.what()));
    }
}
} // namespace bridge
