// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::fastPoll() {
    fastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    nextPoll = {};
}
upnp::QueueOwnershipIO QueueRoom::Impl::ownershipIO() {
    upnp::QueueOwnershipIO io;
    io.queueTransport = "x-rincon-queue:" + room.uuid + "#0";
    io.browse = [this] { return coordinator.browse(); };
    io.owned = [this](const auto &uri) { return owned.count(uri) != 0; };
    io.playlists = [this] { return coordinator.playlists(); };
    io.save = [this](const auto &title) { return actions.save(title).response.value("AssignedObjectID"); };
    io.deletePlaylist = [this](const auto &id) { coordinator.destroyPlaylist(id); };
    io.transport = [this] {
        return coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response.value("CurrentURI");
    };
    io.prepareTransport = [this] {
        const auto ownQueue = "x-rincon-queue:" + room.uuid + "#0";
        const auto uri = coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response.value("CurrentURI");
        if (uri == ownQueue) return;
        actions.selectQueue(room.uuid);
        if (coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response.value("CurrentURI") != ownQueue)
            throw std::runtime_error("queue-transport-switch-unconfirmed");
        queueSelectedForAdmission = true;
        logQueue("admission transport switched and confirmed before transport settings");
    };
    io.crossfade = [this] { return actions.crossfade().response.value("CrossfadeMode") == "1"; };
    io.setCrossfade = [this](bool value) {
        try {
            actions.crossfade(value);
        } catch (const std::exception &e) {
            const std::string error = e.what();
            if (releasing || queueSelectedForAdmission || error.find("fault=7") == std::string::npos)
                throw;
            // A setting was refused before our first switch, possibly after a source race.
            actions.selectQueue(room.uuid);
            if (coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response.value("CurrentURI") !=
                "x-rincon-queue:" + room.uuid + "#0")
                throw std::runtime_error("queue-transport-switch-unconfirmed");
            queueSelectedForAdmission = true;
            logQueue("admission retry after confirmed queue switch: " + error);
            actions.crossfade(value); // Exactly one retry; a post-switch failure faults the room.
        }
        if (actions.crossfade().response.value("CrossfadeMode") != (value ? "1" : "0"))
            throw std::runtime_error("queue-crossfade-unconfirmed");
    };
    io.remove = [this](auto update, auto first, auto count) { actions.remove(update, first, count); };
    io.stop = [this] { actions.stop(); };
    io.log = logQueue;
    return io;
}
void QueueRoom::Impl::releaseWindowParent(uint64_t token, uint64_t before) {
    if (before == UINT64_MAX) speakerNavFirst.erase(token);
    std::lock_guard<std::mutex> lock(sourceMutex);
    for (const auto &source : sourceTokens)
        if (source.second == token && source.first < before && source.first < UINT64_MAX / 2)
            queue.releaseParent(source.first);
}
void QueueRoom::Impl::release(bool stop) {
    refreshUpcoming = false;
    lmsNavigation = false;
    navigationDeadline = {};
    navigationItem.reset();
    publishedWindowIndex.reset();
    auto cleanupOwned = std::move(owned);
    owned.clear();
    std::set<uint64_t> parents;
    for (const auto &item : items) {
        parents.insert(item.owner ? item.owner : item.stream);
        if (item.owner && !item.segmentOffset) parents.insert(item.stream);
    }
    for (auto parent : parents) queue.releaseParent(parent);
    std::set<uint64_t> tokens;
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        for (auto parent : parents) {
            const auto token = sourceTokens.find(parent);
            if (token != sourceTokens.end()) tokens.insert(token->second);
        }
    }
    for (auto token : tokens) releaseWindowParent(token);
    for (const auto &lazy : lazyItems) lazy.second->cancel();
    lazyItems.clear();
    selections.clear();
    selectionOrder.clear();
    speakerNavCommands.clear();
    speakerNavFirst.clear();
    replacementReasons.clear();
    items.clear();
    releasing = true;
    ownership.release(!sourceAway,
                      [cleanupOwned = std::move(cleanupOwned)](const auto &uri) { return cleanupOwned.count(uri) != 0; }, stop);
    releasing = false;
    started = false;
    waitingForPlay = false;
    current = 0;
    lastElapsed = 0;
    lastPositionAt = {};
    state = "STOPPED";
    publish();
}
void QueueRoom::Impl::command(char c, uint64_t next) {
    const bool navigationTimeout = c == 'Z';
    if (navigationTimeout) c = 'q';
    if (c == 'H') {
        const auto snapshot = reader.read(host, mac);
        const auto audible = std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.stream == current; });
        if (snapshot.mode == "pause" && audible != items.end() &&
            snapshot.index < snapshot.entries.size() && snapshot.entries[snapshot.index].url == audible->sourceUrl) {
            pausedFlushRetained = true;
            logQueue("pause flush retained: LMS mode=pause same source, queue and spool kept");
            return;
        }
        finitePause = false;
        queue.flush();
        command('q', next);
        return;
    }
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
        const auto source = audible != items.end() && audible->owner ? audible->owner : current;
        const auto oldRunElapsed = queue.audibleElapsedMs(source).value_or(0);
        const bool sameClock = std::abs(int64_t(snapshot.timeMs) - int64_t(resumeSnapshot.timeMs)) <= 500;
        // Native LMS can expose the new seek offset plus the preceding run's
        // STAT clock until our cached STMs reanchors that same source. Verify
        // that exact clock equation, rather than widening the seek tolerance.
        const bool pendingNativeClock = pausedFlushRetained && pathReason == "remote-finite" &&
            std::abs(int64_t(snapshot.timeMs) - int64_t(oldRunElapsed) - int64_t(resumeSnapshot.timeMs)) <= 500;
        const bool same =
            known && audible != items.end() && snapshot.index == resumeSnapshot.index &&
            snapshot.index < snapshot.entries.size() && snapshot.index < resumeSnapshot.entries.size() &&
            snapshot.entries[snapshot.index].url == audible->sourceUrl &&
            snapshot.entries[snapshot.index].url == resumeSnapshot.entries[snapshot.index].url &&
            (sameClock || pendingNativeClock);
        logQueue("resume echo decision=" + std::string(same ? "adopt" : "replace") +
                 " lms_time_ms=" + std::to_string(snapshot.timeMs) +
                 " saved_time_ms=" + std::to_string(resumeSnapshot.timeMs) +
                 " old_run_elapsed_ms=" + std::to_string(oldRunElapsed) +
                 " pending_native_clock=" + std::to_string(pendingNativeClock));
        pausedFlushRetained = false;
        if (same) {
            queue.restartCached();
            queue.confirmCachedStart();
            fastPoll();
            logQueue("resume echo adopted: same LMS item/time, retained audio, no renderer restart");
        } else {
            queue.flush();
            command('Z', next); // Existing cached-resume mismatch replacement.
            queue.resumeReceivedStart();
            logQueue("resume echo changed item/time: honour LMS selection");
        }
        return;
    }
    if (c == 'q') {
        if (!navigationTimeout && started && !externalReleased && !sourceAway) {
            // A controller navigation starts a fresh producer (q+s), unlike
            // decode-ahead's natural s. Wait for the paired start; its status
            // read verifies the target before any speaker edit.
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                navigationFrom = publishedWindowIndex.value_or(playlist.index);
                for (const auto &entry : items) if (entry.stream == current) {
                    const auto owner = entry.owner ? entry.owner : entry.stream;
                    const auto index = sourceIndices.find(owner);
                    if (index != sourceIndices.end()) {
                        const auto selection = selections.find(index->second);
                        if (selection == selections.end() || selection->second.phase != Selection::Producing)
                            navigationFrom = index->second;
                    }
                }
                navigationEdits = queueEdits;
                lmsNavigation = true;
                remoteNaturalSuccessor = false;
                finitePause = false;
                pausedFlushRetained = false;
                handoffAt = {};
                navigationFirst = next;
                navigationItem.reset();
                navigationDeadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(queue_audio::QueueWindow::settleMs);
                stoppedPlay = true; // The next s describes snapshot.index itself.
                ownUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                return;
            }
        }
        remoteNaturalSuccessor = false;
        finitePause = false;
        pausedFlushRetained = false;
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
        const auto owner = record.parent ? record.parent : record.source.occurrence;
        if (owner < firstStream || (record.source.occurrence > UINT64_MAX / 2 && !record.parent))
            continue;
        if (record.item && !record.incomplete.empty())
            continue; // LMS cancelled this render, not a speaker rejection.
        if (!record.fault.empty())
            { if (record.storageFault) throw std::runtime_error(record.fault); throw TrackFault(record.fault); }
        if (record.pairedFrom && record.actual - record.attachOffset < record.verificationFrames)
            continue; // Verify the complete overlap before exposing an attach.
        if (!record.item || !record.incomplete.empty())
            continue;
        if (!record.parent) {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto token = sourceTokens.find(owner);
            if (token != sourceTokens.end()) {
                const auto first = speakerNavFirst.find(token->second);
                if (first != speakerNavFirst.end() && owner < first->second)
                    continue; // Our selection cancels decode-ahead; only its paired producer may bind.
            }
            if (token != sourceTokens.end() && !window.find(token->second)) {
                queue.releaseParent(owner);
                continue; // An LMS edit removed this producer before publication.
            }
        }
        if (std::any_of(items.begin(), items.end(),
                        [&](const auto &i) { return i.stream == record.source.occurrence || i.owner == record.source.occurrence; }))
            continue;
        if (lmsNavigation && owner < navigationFirst)
            continue; // A superseded producer cannot enlarge the controller's navigation window.
        if (record.source.rate > queue.maxSampleRate())
            throw TrackFault("queue-rate-unsupported rate=" + std::to_string(record.source.rate) +
                                     " cap=" + std::to_string(queue.maxSampleRate()));
        std::optional<uint64_t> length;
        try {
            length = record.item->contentLength();
        } catch (const queue_audio::PublicationSuperseded &) {
            continue; // Storage published this pending item after snapshot(); retry the new immutable item.
        } catch (const queue_audio::ItemCancelled &) {
            continue;
        }
        if (!length)
            continue;
        if (!record.fault.empty())
            { if (record.storageFault) throw std::runtime_error(record.fault); throw TrackFault(record.fault); }
        if (!record.parent) {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto token = sourceTokens.find(owner);
            if (token != sourceTokens.end()) {
                const auto lazy = lazyItems.find(token->second);
                const auto selection = selections.find(sourceIndices.at(owner));
                const auto speakerSelection = selection != selections.end() && selection->second.token == token->second &&
                    selection->second.phase == Selection::Producing && speakerNavCommands[token->second];
                if (!lmsNavigation || owner < navigationFirst || token->second != navigationToken || speakerSelection) {
                    auto cached = std::find_if(items.begin(), items.end(), [&](const auto &i) {
                        const auto identity = sourceTokens.find(i.owner ? i.owner : i.stream);
                        return i.item && !i.segmentOffset && identity != sourceTokens.end() &&
                               identity->second == token->second;
                    });
                    if (cached != items.end() && record.source.occurrence < cached->stream)
                        continue; // A newer producer already owns this retained URI's clock.
                    if (cached != items.end() && cached->item == record.item && !cached->originMs) {
                        // After LMS Previous, natural decode-ahead may revisit
                        // a retained upcoming parent. Bind its new producer
                        // clock to the existing URI instead of appending it.
                        if (lmsNavigation && token->second == navigationToken && speakerSelection) {
                            lmsNavigation = navigationSelecting = false;
                            navigationDeadline = {};
                            navigationItem.reset();
                            logQueue("window pairing lms_index=" + std::to_string(sourceIndices.at(owner)) +
                                " decision=paired lms_commands=1");
                        }
                        if (!cached->owner) cached->owner = cached->stream;
                        if (current == cached->stream) current = record.source.occurrence;
                        cached->stream = record.source.occurrence;
                        selectionPublished(sourceIndices.at(owner));
                        windowDirty = true;
                        continue;
                    }
                }
                auto entry = std::find_if(items.begin(), items.end(), [&](const auto &i) {
                    return !i.item && sourceTokens[i.stream] == token->second;
                });
                if (!lmsNavigation && lazy != lazyItems.end() && entry != items.end()) {
                    if (!lazy->second->publish(record.item)) {
                        queue.releaseParent(owner);
                        selections.erase(sourceIndices.at(owner));
                        continue;
                    }
                    entry->stream = record.source.occurrence;
                    entry->item = record.item;
                    entry->frames = (*length - 44) / 6;
                    entry->originMs = record.originMs;
                    if (speakerSelection) {
                        const auto reason = replacementReasons.find(token->second);
                        if (reason != replacementReasons.end()) {
                            logQueue("window target re-produced lms_index=" + std::to_string(sourceIndices.at(owner)) +
                                " origin_ms=" + std::to_string(record.originMs) + " lms_commands=1 reason=" + reason->second);
                            logQueue("window pairing lms_index=" + std::to_string(sourceIndices.at(owner)) +
                                " decision=re-produced reason=" + reason->second + " lms_commands=1");
                            replacementReasons.erase(reason);
                        } else logQueue("window pairing lms_index=" + std::to_string(sourceIndices.at(owner)) +
                            " decision=paired lms_commands=1");
                    }
                    logQueue("lazy bound lms_index=" + std::to_string(sourceIndices[owner]) +
                        " lms_commands=" + std::to_string(speakerNavCommands[token->second]));
                    selectionPublished(sourceIndices.at(owner));
                    windowDirty = true;
                    continue;
                }
            }
        }
        queue_audio::PlaylistEntry track;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            auto found = sourceTracks.find(record.parent ? record.parent : record.source.occurrence);
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
            queueSelectedForAdmission = false;
            ownership.acquire(title);
            logQueue("ownership acquired");
            publish();
        }
        std::vector<std::string> expected;
        for (const auto &i : items) expected.push_back(i.uri);
        const auto selection = selections.find(navigationFrom);
        const bool retainedRewind = lmsNavigation && owner >= navigationFirst && !record.parent &&
            record.sourceToken == navigationToken &&
            selection != selections.end() && selection->second.token == navigationToken && selection->second.rewindMs &&
            std::any_of(items.begin(), items.end(), [&](const auto &entry) { return entry.item == record.item; });
        upnp::QueueSnapshot before;
        if (!retainedRewind) {
            before = coordinator.browse();
            if (before.uris != expected) throw std::runtime_error("queue-changed-while-owned");
        }
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
        metadata.preciseDuration = record.estimated;
        const auto changes = upnp::sanitizeXmlFields({{"title", &metadata.title},
                                                      {"artist", &metadata.artist},
                                                      {"album", &metadata.album},
                                                      {"artwork", &metadata.artwork}});
        if (!changes.empty())
            logQueue("metadata sanitized mode=queue occurrence=" + std::to_string(record.source.occurrence) +
                     " " + changes);
        owned.insert(metadata.url); // include an enqueue whose reply is lost
        size_t insertion = items.size();
        if (record.parent) {
            auto previous = std::find_if(items.rbegin(), items.rend(), [&](const auto &i) {
                return i.stream == record.parent || i.owner == record.parent;
            });
            if (previous == items.rend()) throw std::runtime_error("continuation predecessor missing");
            insertion = size_t(std::distance(items.begin(), previous.base()));
        }
        if (record.parent) logQueue("continuation insertion attempted parent=" + std::to_string(record.parent) +
                                    " occurrence=" + std::to_string(record.source.occurrence));
        upnp::SoapResult result;
        const auto didl = upnp::queueDidl(metadata);
        if (lmsNavigation && owner >= navigationFirst && !record.parent) {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto identity = sourceTokens.find(owner);
            if (identity != sourceTokens.end() && identity->second == navigationToken) {
                // Admission can race the CLI selection. The generation is a
                // lower bound; the occurrence token identifies its producer.
                navigationFirst = owner;
                navigationItem = Entry{record.source.occurrence, metadata.url, record.item, track.url,
                                      metadata.frames, metadata.title, 0, 0, didl, record.originMs};
                windowDirty = true;
                nextWindowPoll = {};
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!commands.empty()) return; // A controller move supersedes this prepared append.
        }
        try {
            result = actions.add(metadata.url, didl,
                                 insertion == items.size() ? 0 : unsigned(insertion + 1));
        } catch (const std::exception &e) {
            if (record.parent) logQueue("continuation insertion failed parent=" + std::to_string(record.parent) +
                                       " reason=" + e.what());
            throw;
        }
        if (record.parent) {
            double remaining = double(record.segmentOffset) / metadata.rate;
            const auto playing = std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.stream == current; });
            if (playing != items.end() && (playing->stream == record.parent || playing->owner == record.parent))
                remaining -= double(playing->segmentOffset) / metadata.rate + lastElapsed;
            const auto margin = remaining - 10;
            logQueue(std::string(margin < 0 ? "WARNING " : "") + "continuation inserted parent=" +
                     std::to_string(record.parent) + " decision=" + record.continuationDecision + " prefetch_margin_s=" + std::to_string(margin));
        }
        logQueue("item occurrence=" + std::to_string(record.source.occurrence) +
                 " title=" + RoomStatus::quote(metadata.title));
        expected.insert(expected.begin() + insertion, metadata.url);
        const auto after = coordinator.browse();
        if (result.response.value("NumTracksAdded") != "1" || after.uris != expected ||
            after.update == before.update)
            {
                if (record.parent) logQueue("continuation insertion failed reason=queue-enqueue-unconfirmed parent=" + std::to_string(record.parent));
                throw std::runtime_error("queue-enqueue-unconfirmed");
            }
        if (record.parent) logQueue("continuation insertion confirmed parent=" + std::to_string(record.parent) +
                                   " occurrence=" + std::to_string(record.source.occurrence));
        items.insert(items.begin() + insertion, {record.source.occurrence, metadata.url, record.item, track.url,
                                                metadata.frames, metadata.title, record.parent, record.segmentOffset, didl});
        windowDirty = true;
        publish();
        if (!started) {
            if (!queueSelectedForAdmission) actions.selectQueue(room.uuid);
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
    logQueue("end decision=fault occurrence=" + std::to_string(current) + " reason=" + reason);
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
void QueueRoom::Impl::trimHistory() {
    if (!started || faulted || externalReleased || sourceAway || !current || lmsNavigation) return;
    using Edits = queue_audio::WindowEdits;
    std::vector<Edits::Parent> parents;
    uint64_t audible = 0;
    for (const auto &item : items) {
        const auto parent = item.owner ? item.owner : item.stream;
        if (parents.empty() || parents.back().token != parent) parents.push_back({parent, {}});
        parents.back().uris.push_back(item.uri);
        if (item.stream == current) audible = parent;
    }
    const auto playing = std::find_if(parents.begin(), parents.end(),
        [audible](const auto &p) { return p.token == audible; });
    if (playing == parents.end()) return;
    const auto history = size_t(playing - parents.begin());
    if (history <= queue_audio::QueueWindow::historyCount) return;
    const auto count = history - queue_audio::QueueWindow::historyCount;
    std::vector<Edits::Parent> wanted(parents.begin() + count, parents.end());
    Edits::IO io;
    io.browse = [this] {
        const auto snapshot = coordinator.browse();
        return Edits::Snapshot{snapshot.update, snapshot.uris};
    };
    io.remove = [this](auto update, auto first, auto n) { actions.remove(update, first, n); };
    io.insert = [](auto, auto, auto) { throw std::logic_error("history trim attempted insertion"); };
    io.log = [this, &parents](const auto &message) {
        std::string indices;
        std::lock_guard<std::mutex> lock(sourceMutex);
        for (const auto &parent : parents) {
            const auto found = sourceIndices.find(parent.token);
            indices += (indices.empty() ? "" : ",") +
                (found == sourceIndices.end() ? std::string("unknown") : std::to_string(found->second));
        }
        logQueue(message + " lms_indices=" + indices + " reason=history-limit");
    };
    Edits(std::move(io)).apply(parents, wanted, audible);
    for (size_t n = 0; n < count; ++n) {
        const auto token = parents[n].token;
        uint64_t identity = 0;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto found = sourceTokens.find(token);
            if (found != sourceTokens.end()) identity = found->second;
        }
        if (identity) releaseWindowParent(identity);
        for (const auto &item : items)
            if (item.owner == token && !item.segmentOffset) queue.releaseParent(item.stream);
        for (const auto &uri : parents[n].uris) owned.erase(uri);
        items.erase(std::remove_if(items.begin(), items.end(), [token](const auto &item) {
            return (item.owner ? item.owner : item.stream) == token;
        }), items.end());
        queue.releaseParent(token);
    }
    publish();
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
                if (c.first == 'L') activateLazy(c.second);
                else if (c.first == 'I')
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
            if (navigationDeadline != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() >= navigationDeadline) {
                navigationDeadline = {};
                if (selecting || selectionDeferred.load() || std::any_of(selections.begin(), selections.end(),
                    [](const auto &selection) { return selection.second.phase != Selection::Published; })) {
                    navigationDeadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(queue_audio::QueueWindow::settleMs);
                    try {
                        const auto snapshot = reader.read(host, mac);
                        if (snapshot.mode == "stop") {
                            const auto fresh = reader.read(host, mac);
                            if (fresh.mode == "stop") {
                                command('Z', navigationFirst); // Confirmed user stop cancels owned requests.
                                if (snapshot.entries.empty() && fresh.entries.empty())
                                    logQueue("window rebuild reason=clear lms_commands=0");
                            }
                        }
                    } catch (const std::exception &) { logQueue("window snapshot unavailable reason=cli-read"); }
                } else {
                    // A standalone q is explicit LMS stop authority. Status can
                    // lag it, but an empty status alone is never clear authority.
                    try {
                        const auto snapshot = reader.read(host, mac);
                        if (snapshot.entries.empty()) {
                            const auto fresh = reader.read(host, mac);
                            if (fresh.entries.empty()) {
                                lmsNavigation = navigationSelecting = false;
                                logQueue("window rebuild reason=clear lms_commands=0");
                                logQueue("window lms-nav from_index=" + std::to_string(navigationFrom) +
                                    " to_index=0 action=rebuild history=0 lms_commands=0 kind=clear");
                            } else logQueue("window snapshot rejected reason=unconfirmed-clear");
                        }
                    } catch (const std::exception &) { logQueue("window snapshot unavailable reason=cli-read"); }
                    command('Z', navigationFirst);
                }
            }
            if (deferredStop != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() >= deferredStop) {
                deferredStop = {};
                resumeEchoUntil = 0;
                resumeFlush = false;
                queue.flush();
                command('Z', deferredNext); // Resume echo expired without a start.
            }
            if (const auto error = queue.workerError(); !error.empty()) {
                roomFault("queue-worker: " + error);
                continue;
            }
            observe();
            append();
            advanceSelections();
            trimHistory();
            reconcileWindow();
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
