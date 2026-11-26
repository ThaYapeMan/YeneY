// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
#include "wav_writer.h"
namespace bridge {
namespace {
// Allow a speaker to recover from a short radio interruption. End the room
// only after 20 s without any evidence that it can still communicate.
constexpr auto speakerUnreachableAfter = std::chrono::seconds(20);
}
std::chrono::milliseconds QueueRoom::Impl::speakerSilence() const {
    auto last = Clock::time_point(Clock::duration(lastSpeakerLife->load()));
    last = std::max(last, speaker->lastRoomAnswer());
    if (last == Clock::time_point{}) last = firstUnanswered;
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last);
}
unsigned QueueRoom::Impl::speakerReadDeadline(unsigned timeout) {
    readStarted = Clock::now();
    if (!releasing && started) queue.extrapolateSpeakerClock(true);
    if (releasing || !unansweredReads) return timeout;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(speakerUnreachableAfter) - speakerSilence();
    return unsigned(std::clamp<int64_t>(remaining.count(), 1, timeout));
}
void QueueRoom::Impl::waitForSpeakerRead(const upnp::QueueReadUnanswered &error) {
    if (releasing) throw;
    if (++unansweredReads == 1) {
        firstUnanswered = readStarted;
        logQueue("speaker silent action=" + error.action + " error=" + error.what());
    }
    if (speakerSilence() >= speakerUnreachableAfter) throw SpeakerUnreachable{};
    publish();
    const auto backoff = std::chrono::milliseconds(250 * (1u << std::min(unansweredReads - 1, 2u)));
    const auto timeLeft = speakerUnreachableAfter - speakerSilence();
    std::unique_lock<std::mutex> lock(mutex);
    if (changed.wait_for(lock, std::min(Clock::duration(backoff), Clock::duration(timeLeft)), [this] { return closing; }))
        throw QueueRoomClosing{};
    if (speakerSilence() >= speakerUnreachableAfter) throw SpeakerUnreachable{};
}
void QueueRoom::Impl::speakerAnswered() {
    const auto now = Clock::now();
    speakerActivity()();
    queue.extrapolateSpeakerClock(false);
    if (!unansweredReads) return;
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - firstUnanswered).count();
    logQueue("speaker answering silent_ms=" + std::to_string(duration) + " unanswered=" + std::to_string(unansweredReads));
    unansweredReads = 0;
    recoveredSpeakerPosition = true;
    publish();
}
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
    io.deletePlaylist = [this](const auto &id) {
        if (speaker->speaker().coordinator != room.name) {
            coordinator.invalidateIdentity();
            logQueue("backup retention deferred reason=ownership-changed");
            return;
        }
        coordinator.destroyPlaylist(id);
    };
    io.transport = [this] {
        return coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response.value("CurrentURI");
    };
    io.prepareTransport = [this] {
        const auto ownQueue = "x-rincon-queue:" + room.uuid + "#0";
        const bool switched = coordinator.read([&] {
            const auto uri = coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response.value("CurrentURI");
            if (uri == ownQueue) return false;
            actions.selectQueue(room.uuid);
            return true;
        });
        if (!switched) return;
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
    if (before == UINT64_MAX) rowOwner.retireAppSelection(token);
    std::lock_guard<std::mutex> lock(sourceMutex);
    for (const auto source : rowOwner.earlierSources(token, before))
        queue.releaseParent(source);
}
void QueueRoom::Impl::release(bool stop, bool cleanupSpeaker) {
    finishSnapshotRefusal("released");
    queue.extrapolateSpeakerClock(false);
    const auto &items = rowOwner.layout();
    generationRebuildEdits.reset();
    refreshUpcoming = false;
    lmsNavigation = false;
    navigationDeadline = {};
    rowOwner.clearNavigationTarget();
    publishedWindowIndex.reset();
    for (const auto &uri : owned) route.retire(uri);
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
            const auto token = rowOwner.sourceToken(parent);
            if (token) tokens.insert(*token);
        }
    }
    for (auto token : tokens) releaseWindowParent(token);
    rowOwner.retireLazyRows();
    rowOwner.clearSelections();
    {
        std::lock_guard<std::mutex> lock(mutex);
        rowOwner.retireCommandStart();
    }
    rowOwner.clearAdmissions();
    rowOwner.clearTransitionNotes();
    rowOwner.commitLayout({});
    releasing = true;
    ownership.release(cleanupSpeaker && !sourceAway,
                      [cleanupOwned = std::move(cleanupOwned)](const auto &uri) { return cleanupOwned.count(uri) != 0; }, stop);
    releasing = false;
    coordinator.invalidateIdentity();
    started = false;
    recoveredSpeakerPosition = false;
    waitingForPlay = false;
    current = 0;
    lastElapsed = 0;
    lastPositionAt = {};
    state = "STOPPED";
    publish();
}
void QueueRoom::Impl::command(char c, uint64_t next) {
    const auto &items = rowOwner.layout();
    const bool navigationTimeout = c == 'Z';
    if (navigationTimeout) c = 'q';
    if (c == 'H') {
        const auto snapshot = readPlaylist();
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
            snapshot = readPlaylist();
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
        rowOwner.clearAdmissions();
        if (!navigationTimeout && started && !externalReleased && !sourceAway) {
            // A controller navigation starts a fresh producer (q+s), unlike
            // decode-ahead's natural s. Wait for the paired start; its status
            // read verifies the target before any speaker edit.
            {
                std::lock_guard<std::mutex> lock(sourceMutex);
                navigationFrom = publishedWindowIndex.value_or(playlist.index);
                for (const auto &entry : items) if (entry.stream == current) {
                    const auto owner = entry.owner ? entry.owner : entry.stream;
                    const auto index = rowOwner.sourceIndex(owner);
                    if (index) {
                        const auto selection = rowOwner.selectionAt(*index);
                        if (!selection || selection->phase != Selection::Producing)
                            navigationFrom = *index;
                    }
                }
                navigationEdits = queueEdits;
                lmsNavigation = true;
                remoteNaturalSuccessor = false;
                finitePause = false;
                pausedFlushRetained = false;
                handoffAt = {};
                rowOwner.beginNavigation(next);
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
    const auto &items = rowOwner.layout();
    if (!paths.isRelay() || faulted || externalReleased || (started && sourceAway))
        return;
    for (const auto &record : queue.snapshot()) {
        const auto owner = record.parent ? record.parent : record.source.occurrence;
        if (owner < firstStream || (record.source.occurrence > UINT64_MAX / 2 && !record.parent))
            continue;
        using Production = queue_audio::QueueRowOwner::Production;
        switch (rowOwner.producerOutcome(record)) {
        case Production::Pending:
        case Production::Cancelled:
        case Production::Evicted:
            continue;
        case Production::Faulted:
            if (record.storageFault) throw std::runtime_error(record.fault);
            throw TrackFault(record.fault);
        case Production::Writing:
        case Production::Complete:
            break;
        }
        if (!record.parent) {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto token = rowOwner.sourceToken(owner);
            if (token) {
                if (rowOwner.predatesAppSelection(*token, owner))
                    continue; // Our selection cancels decode-ahead; only its paired producer may bind.
            }
            if (token && !window.find(*token)) {
                queue.releaseParent(owner);
                continue; // An LMS edit removed this producer before publication.
            }
        }
        if (std::any_of(items.begin(), items.end(),
                        [&](const auto &i) { return i.stream == record.source.occurrence || i.owner == record.source.occurrence; }))
            continue;
        if (lmsNavigation && owner < rowOwner.navigationFirst())
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
        if (!record.parent) {
            std::unique_lock<std::mutex> lock(sourceMutex);
            const auto token = rowOwner.sourceToken(owner);
            if (token) {
                const auto lazy = rowOwner.lazyRow(*token);
                const auto index = rowOwner.sourceIndex(owner).value();
                const auto selection = rowOwner.selectionAt(index);
                const auto speakerSelection = selection && selection->token == *token &&
                    selection->phase == Selection::Producing && rowOwner.appCommands(*token);
                if (!lmsNavigation || owner < rowOwner.navigationFirst() || *token != rowOwner.navigationToken() || speakerSelection) {
                    auto cached = std::find_if(items.begin(), items.end(), [&](const auto &i) {
                        const auto identity = rowOwner.sourceToken(i.owner ? i.owner : i.stream);
                        return i.item && !i.segmentOffset && identity &&
                               *identity == *token;
                    });
                    if (cached != items.end() && cached->item->evicted()) {
                        // Eviction can follow the last HTTP response before
                        // periodic cleanup. A repeat-wrap producer must bind
                        // this existing lazy row, never append a duplicate.
                        lock.unlock();
                        discardEvicted(size_t(cached - items.begin()));
                        lock.lock();
                        cached = items.end();
                    }
                    if (cached != items.end() && record.source.occurrence < cached->stream)
                        continue; // A newer producer already owns this retained URI's clock.
                    if (cached != items.end() && cached->item == record.item && !cached->originMs) {
                        const bool clockChanged = cached->stream != record.source.occurrence;
                        // After LMS Previous, natural decode-ahead may revisit
                        // a retained upcoming parent. Bind its new producer
                        // clock to the existing URI instead of appending it.
                        if (lmsNavigation && *token == rowOwner.navigationToken() && speakerSelection) {
                            lmsNavigation = navigationSelecting = false;
                            navigationDeadline = {};
                            rowOwner.clearNavigationTarget();
                            logQueue("window pairing lms_index=" + std::to_string(index) +
                                " decision=paired lms_commands=1");
                        }
                        if (current == cached->stream) current = record.source.occurrence;
                        rowOwner.rebindClock(size_t(cached - items.begin()), record.source.occurrence);
                        selectionPublished(index);
                        windowDirty = true;
                        if (clockChanged) {
                            lock.unlock();
                            publish();
                        }
                        continue;
                    }
                }
                auto entry = std::find_if(items.begin(), items.end(), [&](const auto &i) {
                    return !i.item && rowOwner.sourceToken(i.stream).value_or(0) == *token;
                });
                if (!lmsNavigation && lazy && entry != items.end()) {
                    if (!rowOwner.reproduce(size_t(entry - items.begin()), *token, record, (*length - 44) / 6)) {
                        queue.releaseParent(owner);
                        rowOwner.retireSelection(index);
                        continue;
                    }
                    {
                        const auto reason = rowOwner.replacementReason(*token);
                        if (reason) {
                            const auto commands = std::to_string(rowOwner.appCommands(*token));
                            if (*reason != "evicted")
                                logQueue("window target re-produced lms_index=" + std::to_string(index) +
                                    " origin_ms=" + std::to_string(record.originMs) + " lms_commands=" + commands + " reason=" + *reason);
                            logQueue("window pairing lms_index=" + std::to_string(index) +
                                " decision=re-produced reason=" + *reason + " lms_commands=" + commands);
                            rowOwner.clearReplacement(*token);
                        } else if (speakerSelection) logQueue("window pairing lms_index=" + std::to_string(index) +
                            " decision=paired lms_commands=1");
                    }
                    logQueue("lazy bound lms_index=" + std::to_string(index) +
                        " lms_commands=" + std::to_string(rowOwner.appCommands(*token)));
                    selectionPublished(index);
                    windowDirty = true;
                    continue;
                }
            }
        }
        queue_audio::PlaylistEntry track;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto found = rowOwner.sourceTrack(record.parent ? record.parent : record.source.occurrence);
            if (!found)
                throw std::runtime_error("queue-stream-metadata-missing");
            track = *found;
            remote[record.source.occurrence] = false;
        }
        if (!ownership.active()) {
            auto now = std::time(nullptr);
            std::tm tm{};
            localtime_r(&now, &tm);
            char title[64];
            std::strftime(title, sizeof title, "Before YeneY %Y-%m-%d %H:%M", &tm);
            queueSelectedForAdmission = false;
            ownership.acquire(title, true);
            logQueue("ownership acquired");
            publish();
        }
        std::vector<std::string> expected;
        for (const auto &i : items) expected.push_back(i.uri);
        const auto selection = rowOwner.selectionAt(navigationFrom);
        const bool retainedRewind = lmsNavigation && owner >= rowOwner.navigationFirst() && !record.parent &&
            record.sourceToken == rowOwner.navigationToken() &&
            selection && selection->token == rowOwner.navigationToken() && selection->rewindMs &&
            std::any_of(items.begin(), items.end(), [&](const auto &entry) { return entry.item == record.item; });
        const bool ownedNavigation = lmsNavigation && owner >= rowOwner.navigationFirst() && !record.parent &&
            record.sourceToken == rowOwner.navigationToken() && ownership.active();
        upnp::QueueSnapshot before;
        if (!retainedRewind && !ownedNavigation) {
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
        const auto &descriptor = record.item->descriptor();
        metadata.frames = (*length - 44) / 6;
        metadata.rate = descriptor.rate;
        metadata.bits = route.bitsFor(descriptor);
        metadata.size = queue_audio::rowLength(route.formatFor(descriptor.kind, descriptor.rate), metadata.frames, metadata.rate, metadata.bits);
        metadata.preciseDuration = record.estimated;
        const auto changes = upnp::sanitizeXmlFields({{"title", &metadata.title},
                                                      {"artist", &metadata.artist},
                                                      {"album", &metadata.album},
                                                      {"artwork", &metadata.artwork}});
        if (!changes.empty())
            logQueue("metadata sanitized mode=queue occurrence=" + std::to_string(record.source.occurrence) +
                     " " + changes);
        if (!record.parent && !lmsNavigation) {
            std::optional<queue_audio::QueueWindow::Slot> slot;
            { std::lock_guard<std::mutex> lock(sourceMutex); slot = window.find(rowOwner.sourceToken(owner).value()); }
            if (slot) {
                auto row = makeRow(*slot, nullptr, metadata.bits, descriptor.rate);
                rowOwner.declaredLazyRow(slot->token).publish(record.item, record.source.occurrence);
                metadata.url = row.uri;
                metadata.session = session + "-" + std::to_string(slot->token);
                if (!record.estimated && std::isfinite(slot->track.duration) && slot->track.duration > 0)
                    metadata.playlistDuration = uint64_t(slot->track.duration);
            }
        }
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
        if (lmsNavigation && owner >= rowOwner.navigationFirst() && !record.parent) {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto identity = rowOwner.sourceToken(owner);
            if (identity && rowOwner.prepareNavigation(*identity,
                {record.source.occurrence, metadata.url, record.item, track.url,
                 metadata.frames, metadata.title, 0, 0, didl, record.originMs})) {
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
        rowOwner.insertRow(insertion, {record.source.occurrence, metadata.url, record.item, track.url,
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
            // Retention is maintenance, after the renderer can begin fetching
            // PCM. Finish it here so later navigation never inherits its RPCs.
            ownership.pruneBackups();
        }
    }
}
void QueueRoom::Impl::roomFault(const std::string &reason, bool member) {
    if (roomStopped) return;
    roomStopped = true;
    const bool unreachable = reason == "speaker-unreachable";
    logQueue("end decision=fault occurrence=" + std::to_string(current) + " reason=" + reason +
             (unreachable ? " silent_ms=" + std::to_string(speakerSilence().count()) : ""));
    fault = reason;
    faulted = true;
    selecting = selectionKnown = false;
    if (unreachable) {
        std::lock_guard<std::mutex> lock(mutex);
        // A natural decode-ahead start can arrive during the unanswered
        // read. Retire it before the LMS stop acknowledgement is queued.
        rowOwner.cancelRequest();
        selectionDeferred = false;
    }
    paths.cancelSelection();
    if (!member) {
        try { release(!externalReleased, !unreachable); }
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
bool QueueRoom::Impl::discardEvicted(size_t row) {
    const auto &entry = rowOwner.layout().at(row);
    if (!entry.item || !entry.item->evicted()) return false;
    std::lock_guard<std::mutex> lock(sourceMutex);
    const auto owner = entry.owner ? entry.owner : entry.stream;
    const auto identity = rowOwner.sourceToken(owner);
    if (!identity) return false;
    const auto token = *identity;
    if (!rowOwner.invalidateAudio(token, row)) return false;
    rowOwner.noteReplacement(token, "evicted");
    logQueue("cache evicted lms_index=" + std::to_string(rowOwner.sourceIndex(owner).value()));
    return true;
}
void QueueRoom::Impl::trimHistory() {
    const auto &items = rowOwner.layout();
    // Rows survive navigation; the item store evicts unprotected played audio.
    if (!started || lmsNavigation) return;
    for (size_t row = 0; row < items.size(); ++row) discardEvicted(row);
}
void QueueRoom::Impl::run() {
    for (;;) {
        std::deque<std::pair<char, uint64_t>> pending;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(activePositionSource == "websocket" ? 20 : 100),
                             [&] { return closing || !commands.empty(); });
            if (closing)
                break;
            pending.swap(commands);
        }
        try {
            if (started && paths.isRelay() && speakerSilence() >= speakerUnreachableAfter)
                throw SpeakerUnreachable{};
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
                if (selecting || selectionDeferred.load() || rowOwner.waitingForSelectionPublication()) {
                    navigationDeadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(queue_audio::QueueWindow::settleMs);
                    try {
                        const auto snapshot = readPlaylist();
                        if (snapshot.mode == "stop") {
                            const auto fresh = readPlaylist();
                            if (fresh.mode == "stop") {
                                command('Z', rowOwner.navigationFirst()); // Confirmed user stop cancels owned requests.
                                if (snapshot.entries.empty() && fresh.entries.empty())
                                    logQueue("window rebuild reason=clear lms_commands=0");
                            }
                        }
                    } catch (const std::exception &) { logQueue("window snapshot unavailable reason=cli-read"); }
                } else {
                    // A standalone q is explicit LMS stop authority. Status can
                    // lag it, but an empty status alone is never clear authority.
                    try {
                        const auto snapshot = readPlaylist();
                        if (snapshot.entries.empty()) {
                            const auto fresh = readPlaylist();
                            if (fresh.entries.empty()) {
                                lmsNavigation = navigationSelecting = false;
                                logQueue("window rebuild reason=clear lms_commands=0");
                                logQueue("window lms-nav from_index=" + std::to_string(navigationFrom) +
                                    " to_index=0 action=rebuild history=0 lms_commands=0 kind=clear");
                            } else logQueue("window snapshot rejected reason=unconfirmed-clear");
                        }
                    } catch (const std::exception &) { logQueue("window snapshot unavailable reason=cli-read"); }
                    command('Z', rowOwner.navigationFirst());
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
            append();
            advanceSelections();
            observe();
            append();
            advanceSelections();
            trimHistory();
            reconcileWindow();
        } catch (const QueueRoomClosing &) {
            break;
        } catch (const SpeakerUnreachable &) {
            roomFault("speaker-unreachable");
            unansweredReads = 0;
            *lastSpeakerLife = 0;
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
