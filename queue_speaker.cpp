// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::externalStop(const std::string &reason) {
    if (externalReleased) return;
    logQueue("end decision=external-stop reason=" + reason + " occurrence=" + std::to_string(current) +
             " source_class=" + takeoverSource + " elapsed_s=" + std::to_string(lastElapsed));
    expectedState = "STOPPED";
    finitePause = false;
    pausedFlushRetained = false;
    remoteNaturalSuccessor = false;
    resumeEchoUntil = 0;
    resumeFlush = false;
    deferredStop = {};

    coordinator.invalidateIdentity();
    externalReleased = true;
    stopAcknowledged = false;
    firstStream = queue.latestOccurrence() + 1;
    sourceAway = sourceAway || reason == "speaker-source-change";
    if (sourceAway) {
        logQueue("external takeover: source=" + takeoverSource);
        logQueue("crossfade restore skipped: transport owned by " + takeoverSource);
    }

    release();
    speakerCommand("stop", reason);
}
void QueueRoom::Impl::observe() {
    const auto live = speaker->speaker();
    if (live.coordinator != room.name && (!stoppedPlay || started || state == "PLAYING" || state == "PAUSED") && !externalReleased) {
        takeoverSource = "other-group member";
        externalStop("speaker-source-change");
        return;
    }
    if (!paths.isRelay()) {
        auto info = speaker->transportInfo();
        std::string uri;
        if (handoffAt != std::chrono::steady_clock::time_point{}) {
            if (!speaker->readTransportInfo(info) || !speaker->currentUri(uri))
                return;
            if (uri != streamUrl())
                return;
        }
        const auto next = info.state == "PLAYING"           ? "PLAYING"
                          : info.state == "PAUSED_PLAYBACK" ? "PAUSED"
                                                            : "STOPPED";
        if (state != next) {
            state = next;
            publish();
        }
        if (state == "PLAYING" && handoffAt != std::chrono::steady_clock::time_point{})
            handoffComplete();
        return;
    }
    if (!started || faulted)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (selecting && std::chrono::steady_clock::now() < cancelledHeaderUntil) return;
    }
    // Producer admission and live lazy requests take priority over routine
    // SOAP polling. Writes still retain their topology/ownership fences.
    if (!selecting && (lmsNavigation || rowOwner.waitingForSelectionPublication())) return;
    const auto pending = [this] {
        std::lock_guard<std::mutex> lock(mutex);
        return !commands.empty();
    };
    const auto now = std::chrono::steady_clock::now();
    const auto selectedPosition = positionSources.select(positionMonotonic());
    auto precise = selectedPosition.first && std::string(selectedPosition.first->precision()) == "ms" ?
        selectedPosition.second : std::nullopt;
    const bool waitingForPosition = precise && precise->held && mapSpeakerState(precise->rawState) != "STOPPED";
    if (precise && precise->held && !waitingForPosition) precise.reset();
    if (precise && rendererObservationAfter && precise->observedAt < rendererObservationAfter)
        return;
    const bool rendererObservationConfirmed = precise && rendererObservationAfter;
    if (rendererObservationConfirmed) rendererObservationAfter = 0;
    const std::string source = selectedPosition.first ? selectedPosition.first->name() : "upnp";
    const bool positionSourceChanged = activePositionSource != source;
    const std::string reason = precise ? (websocketPosition ? websocketPosition->connectionState() : "local-api-ready") : websocketPosition ? websocketPosition->failure() : "configured";
    const bool reasonChanged = activePositionReason != reason;
    activePositionReason = reason;
    const double reasonNow = positionMonotonic();
    if (!precise && websocketPosition && positionFailureLog.due(reasonNow, websocketPosition->failureCount()) && !positionSourceChanged) {
        logQueue("position source=" + source + " reason=" + reason);
    }
    if (reasonChanged && !positionSourceChanged) publish();
    if (positionSourceChanged) {
        activePositionSource = source;
        activePositionPrecision = selectedPosition.first ? selectedPosition.first->precision() : "second";
        queue.positionClock(0, {});
        previousPrecisePosition.reset();
        logQueue("position source=" + source + " reason=" + reason);
        publish();
    }
    bool websocketEvent = false;
    if (websocketPosition) {
        for (const auto &sample : websocketPosition->takeEvents()) {
            logQueue("speaker event itemId=" + sample.item + " position_ms=" + std::to_string(sample.positionMs) +
                " previousItemId=" + sample.previousItem + " previousPositionMillis=" +
                (sample.previousPositionMs ? std::to_string(*sample.previousPositionMs) : "unavailable"));
            const auto length = websocketExactLengths.find(sample.previousItem);
            if (sample.previousPositionMs && length != websocketExactLengths.end() &&
                (sample.item != sample.previousItem || sample.state == "STOPPED") &&
                *sample.previousPositionMs >= length->second - 1000)
                logQueue("speaker finished itemId=" + sample.previousItem + " last_position_ms=" +
                    std::to_string(*sample.previousPositionMs) + " exact_promised_ms=" + std::to_string(length->second));
        }
        const auto diagnosticRevision = websocketPosition->diagnosticRevision();
        if (diagnosticRevision != websocketDiagnosticRevision) {
            websocketDiagnosticRevision = diagnosticRevision;
            publish();
        }
        const auto revision = websocketPosition->revision();
        websocketEvent = revision != websocketRevision;
        websocketRevision = revision;
        if (websocketEvent || now >= nextPositionStatus) {
            nextPositionStatus = now + std::chrono::seconds(1);
            publish();
        }
    }
    if (waitingForPosition) {
        requestStatus();
        return;
    }
    // LMS keeps extrapolating while the room is playing. Refresh the held
    // BUFFERING coordinate without reporting a pause; the core coalesces
    // requested reports at its existing 50 ms limit.
    if (precise && speakerTransitioning(precise->state)) requestStatus();
    const bool event = eventDirty->exchange(false) || websocketEvent;
    if (!event && now < nextPoll)
        return;
    nextPoll = now + std::chrono::milliseconds(precise && state != "STOPPED" ? 20 : now < fastUntil ? 100 : pollMs);
    const bool identityPoll = !precise || event || cachedPosition.empty() || now >= nextIdentityPoll ||
        precise->item != lastWebsocketItem ||
        (precise && previousPrecisePosition && precise->state != previousPrecisePosition->state);
    if (identityPoll) {
    nextIdentityPoll = now + std::chrono::seconds(1);
    const auto positionStarted = std::chrono::steady_clock::now();
    const auto p = actions.position().response;
    auto position = speakerChanges->poll(positionStarted,
        {{"CurrentTrack", p.value("Track")}, {"CurrentTrackURI", p.value("TrackURI")},
         {"RelativeTimePosition", p.value("RelTime")}});
    if (pending()) return;
    const auto transportStarted = std::chrono::steady_clock::now();
    const auto transportRevision = speaker->transportRevision();
    const auto info = coordinator.transport("GetTransportInfo", {{"InstanceID", "0"}}).response;
    speaker->observeTransportPoll(transportRevision, info);
    const auto transportFields = speakerChanges->poll(transportStarted,
        {{"TransportState", info.value("CurrentTransportState")},
         {"TransportStatus", info.value("CurrentTransportStatus")}});
    if (pending()) return;
    const auto mediaStarted = std::chrono::steady_clock::now();
    const auto media = coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response;
    const auto mediaFields = speakerChanges->poll(mediaStarted,
        {{"NumberOfTracks", media.value("NrTracks")}, {"AVTransportURI", media.value("CurrentURI")}});
    cachedPosition = std::move(position);
    cachedTransport = transportFields;
    cachedMedia = mediaFields;
    cachedMediaUri = media.child("CurrentURI");
    if (precise) {
        // The serialized identity requests can span a buffering release.
        // Use the newer clock only for the item whose identity was observed.
        auto fresh = selectedPosition.first->position(positionMonotonic());
        if (fresh && !fresh->held && fresh->item == precise->item) precise = std::move(fresh);
    }
    }
    auto position = cachedPosition;
    const auto &transportFields = cachedTransport;
    const auto &mediaFields = cachedMedia;
    if (precise) lastWebsocketItem = precise->item;
    // Only this exact owned URI identifies the audible item. CurrentTrack and
    // NumberOfTracks may renumber during our edits without a playback boundary.
    const auto uri = position.at("CurrentTrackURI");
    if (precise && !precise->item.empty() && precise->item.find_first_not_of("0123456789") == std::string::npos &&
        precise->item != position.at("CurrentTrack") && !itemDisagreementLogged) {
        logQueue("speaker itemId disagrees with UPnP row; owned URI wins itemId=" + precise->item + " row=" + position.at("CurrentTrack"));
        itemDisagreementLogged = true;
    }
    if (precise && precise->item == position.at("CurrentTrack")) {
        const auto owned = std::find_if(items.begin(), items.end(), [&](const auto &entry) { return entry.uri == uri; });
        if (owned != items.end() && owned->item) {
            const auto &descriptor = owned->item->descriptor();
            if (descriptor.kind == queue_audio::PromiseKind::Exact && descriptor.promisedFrames && descriptor.rate)
                websocketExactLengths[precise->item] = double(*descriptor.promisedFrames) * 1000 / descriptor.rate;
        }
    }
    const auto avUri = mediaFields.at("AVTransportURI");
    if (cachedMediaUri) {
        sourceAway = !avUri.empty() && avUri != "x-rincon-queue:" + room.uuid + "#0";
        takeoverSource = transportSourceClass(avUri);
    }
    if (!uri.empty() && std::none_of(items.begin(), items.end(), [&](const auto &i) { return i.uri == uri; }))
        sourceAway = true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!commands.empty())
            return; // SlimProto arrived while SOAP was in flight.
    }
    const bool buffering = precise && speakerTransitioning(precise->state);
    const auto &polledTransport = transportFields.at("TransportState");
    const bool announcedPlaying = precise && precise->event && speakerPlaying(precise->state) &&
        precise->item == position.at("CurrentTrack") && polledTransport == "TRANSITIONING";
    const auto transport = buffering ? (state == "PAUSED" ? std::string("PAUSED_PLAYBACK") : std::string("PLAYING"))
                                     : announcedPlaying ? "PLAYING" : polledTransport;
    const auto reportedStatus = transportFields.at("TransportStatus");
    // Seek and rebuffer both pass through TRANSITIONING mid-item.
    if (pathReason == "remote-finite" && state == "PLAYING" && transport == "PAUSED_PLAYBACK") {
        const auto stamp = std::chrono::steady_clock::now();
        const auto pausedPosition = actions.position().response;
        if (pausedPosition.value("TrackURI") == uri)
            position = speakerChanges->poll(stamp,
                {{"CurrentTrack", pausedPosition.value("Track")}, {"CurrentTrackURI", uri},
                 {"RelativeTimePosition", pausedPosition.value("RelTime")}});
    }
    if (reportedStatus != transportStatus) {
        transportStatus = reportedStatus;
        logQueue("TransportStatus=" + transportStatus + " uri=" + transportSourceLog(uri));
        publish();
    }
    auto it = std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.uri == uri; });
    if (sourceAway) {
        externalStop("speaker-source-change");
        return; // Neither an error nor the echo guard authorises stopping a foreign source.
    }
    if (!reportedStatus.empty() && reportedStatus != "OK")
        throw std::runtime_error("playback-fault action=Play uri=" + uri +
                                 " TransportStatus=" + reportedStatus);
    if (transport == "TRANSITIONING") return;
    if (waitingForPlay && transport == "STOPPED" &&
        std::chrono::steady_clock::now() - playAt > std::chrono::seconds(3))
        throw std::runtime_error("playback-fault action=Play uri=" + uri +
                                 " TransportStatus=" + reportedStatus + " stopped-after-play");
    auto previousItem =
        std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.stream == current; });
    // A sub-second final continuation may start and finish between polls.
    // Its owned stopped URI is end evidence for the already-audible parent,
    // not a fabricated new LMS track start.
    const bool stoppedContinuation = transport == "STOPPED" && it != items.end() &&
        previousItem != items.end() && it == std::prev(items.end()) && it->owner &&
        it->owner == (previousItem->owner ? previousItem->owner : previousItem->stream);
    if (stoppedContinuation && it != previousItem) {
        previousItem = it;
        lastElapsed = 0; // the previous segment's clock is not this segment's position
    }
    const bool finalItem = previousItem != items.end() && previousItem == std::prev(items.end());
    bool cancelledCurrent = false;
    const auto checked = it != items.end() ? it : previousItem;
    if (current && it != items.end() && it->stream != current && transport == "PLAYING") {
        const auto owner = it->owner ? it->owner : it->stream;
        const auto identity = rowOwner.sourceToken(owner);
        const auto slot = !identity ? std::optional<queue_audio::QueueWindow::Slot>{} : window.find(*identity);
        const auto previousOwner = previousItem == items.end() ? 0 :
            previousItem->owner ? previousItem->owner : previousItem->stream;
        const auto previousIndex = rowOwner.sourceIndex(previousOwner);
        const bool naturalBoundary = previousItem != items.end() && previousItem->item &&
            previousIndex && slot && slot->index == *previousIndex + 1 &&
            previousItem->frames <= (lastElapsed + 3) * previousItem->item->descriptor().rate;
        if (slot && !naturalBoundary) arbitrateSelection(slot->token, "speaker-nav");
    }
    if (checked != items.end() && !checked->item) return;
    if (checked != items.end()) {
        if (discardEvicted(*checked)) return;
        try {
            (void)checked->item->contentLength();
        } catch (const queue_audio::ItemCancelled &) {
            if (discardEvicted(*checked)) return;
            cancelledCurrent = true;
        }
    }
    unsigned ph = 0, pm = 0, ps = 0;
    const bool positionValid = sscanf(position.at("RelativeTimePosition").c_str(), "%u:%u:%u", &ph, &pm, &ps) == 3;
    const uint64_t reportedMs = precise ? uint64_t(precise->positionMs) :
        positionValid ? uint64_t(ph * 3600 + pm * 60 + ps) * 1000 : 0;
    if (!precise && positionValid)
        upnpPosition.update({uri, transport, double(reportedMs), positionMonotonic(), 0});
    const uint64_t reportedElapsed = reportedMs / 1000;
    const bool rendererEcho = precise && rendererSeekPosition &&
        precise->sampleTime - rendererSeekPosition->issued < 3;
    bool rendererPositionConfirmed = rendererObservationConfirmed;
    if (precise && rendererSeekPosition) {
        const auto error = precise->positionMs - rendererSeekPosition->targetMs -
            (speakerPlaying(precise->state) ? std::max(0.0, precise->sampleTime - rendererSeekPosition->issued) * 1000 : 0);
        if (std::abs(error) <= 500) {
            logQueue("seek verification source=" + activePositionSource + " error_ms=" + std::to_string(error) + " tolerance_ms=500");
            rendererSeekPosition.reset();
            rendererPositionConfirmed = true;
        } else if (rendererEcho) {
            // The fitted clock still describes playback before our renderer
            // seek. Do not announce this producer with that old coordinate.
            return;
        } else rendererSeekPosition.reset();
    }
    const bool preciseSeek = precise && !rendererEcho && previousPrecisePosition &&
        precise->item == previousPrecisePosition->item && (speakerPlaying(precise->state) || speakerTransitioning(precise->state)) &&
        speakerPlaying(previousPrecisePosition->state) &&
        std::abs(precise->positionMs - previousPrecisePosition->positionMs -
            (precise->sampleTime - previousPrecisePosition->sampleTime) * 1000) > 150;
    previousPrecisePosition = precise;
    const auto playbackAdvance = lastPositionAt == std::chrono::steady_clock::time_point{} ? 0 :
        std::chrono::duration_cast<std::chrono::seconds>(now - lastPositionAt).count();
    if ((positionValid || precise) && it != items.end() && it == previousItem &&
        (transport == "PLAYING" || (precise && speakerTransitioning(precise->state))) && !lmsNavigation && !navigationSelecting &&
        (precise ? preciseSeek : (reportedElapsed < lastElapsed || reportedElapsed > lastElapsed + 2 + uint64_t(playbackAdvance)))) {
        const auto owner = it->owner ? it->owner : it->stream;
        const auto index = rowOwner.sourceIndex(owner);
        const auto token = rowOwner.sourceToken(owner);
        const auto records = queue.snapshot();
        const auto producer = std::find_if(records.begin(), records.end(), [&](const auto &r) {
            return r.source.occurrence == it->stream;
        });
        if (index && token && producer != records.end()) {
            const auto offset = producer->seekPositionMs;
            // A finite remote WAV starts at its saved source origin even
            // though its informational duration still describes the full item.
            const auto originMs = producer->estimated ? producer->sourceOriginMs : producer->originMs;
            const auto positionMs = uint64_t(originMs) +
                it->segmentOffset * 1000 / it->item->descriptor().rate + reportedMs;
            const bool rewind = positionMs < offset;
            logQueue("speaker-seek lms_index=" + std::to_string(*index) +
                " position_ms=" + std::to_string(positionMs) + " lms_offset_ms=" + std::to_string(offset) +
                " decision=" + (rewind ? "time-command" : "stat") + " lms_commands=" + (rewind ? "1" : "0"));
            if (rewind) {
                rowOwner.enqueueSelection(*index, *token, positionMs / 1000 * 1000);
                advanceSelections();
            }
        }
    }
    if (positionValid || precise) lastPositionAt = now;
    const auto endEvidence = it == previousItem ? std::max(lastElapsed, reportedElapsed) : lastElapsed;
    uint64_t realEnd = finalItem ? previousItem->frames : 0;
    if (finalItem && !cancelledCurrent && previousItem->item->descriptor().kind == queue_audio::PromiseKind::UpperBound &&
        previousItem->item->finished()) realEnd = previousItem->item->producedFrames();
    bool episodeComplete = true;
    uint64_t episodeProduced = 0, episodePlayed = 0;
    bool estimatedEpisode = false;
    if (previousItem != items.end()) {
        const auto owner = previousItem->owner ? previousItem->owner : previousItem->stream;
        for (const auto &record : queue.snapshot()) {
            if (record.source.occurrence != owner && record.parent != owner) continue;
            if (!record.estimated) continue;
            estimatedEpisode = true;
            episodeComplete = episodeComplete && record.productionComplete;
            episodeProduced += record.actual;
        }
        episodePlayed = previousItem->segmentOffset +
            std::min(realEnd, (endEvidence + 1) * previousItem->item->descriptor().rate);
    }
    if (estimatedEpisode && episodeComplete && finalItem && previousItem->owner && realEnd==0)
        episodePlayed=episodeProduced; // empty deadline fallback: preceding real EOF already played
    const bool nearEnd = finalItem &&
        (!estimatedEpisode || (episodeComplete && episodePlayed >= episodeProduced)) &&
        realEnd <= (endEvidence + (estimatedEpisode ? 1 : 2)) * previousItem->item->descriptor().rate;
    const bool naturalEnd = nearEnd && transport == "STOPPED";
    if (naturalEnd && state == "STOPPED") return;
    if (transport == "STOPPED" && state != "STOPPED") {
        logQueue("end decision=" + std::string(nearEnd ? "natural" : estimatedEpisode && episodeProduced > episodePlayed ? "fault-unplayed-audio" : "external-stop") +
                 " occurrence=" + std::to_string(current) + " final=" + std::to_string(finalItem) +
                 " producer_complete=" + std::to_string(episodeComplete) + " produced_frames=" + std::to_string(episodeProduced) +
                 " played_bound_frames=" + std::to_string(episodePlayed) + " elapsed_s=" + std::to_string(endEvidence) +
                 " segment_end_frames=" + std::to_string(realEnd));
        if (estimatedEpisode && !nearEnd && episodeProduced > episodePlayed &&
            previousItem != items.end() && previousItem->frames <= (endEvidence + 1) * previousItem->item->descriptor().rate)
            throw TrackFault("declared boundary stopped with unplayed episode audio");
    }
    const auto transportSource = avUri;
    const bool sourceChanged = cachedMediaUri && transportSource != "x-rincon-queue:" + room.uuid + "#0" &&
                               (!transportSource.empty() || !nearEnd);
    const bool missingItem = it == items.end();
    const bool unsolicited = now >= ownUntil && !waitingForPlay;
    if (unsolicited && expectedState != "STOPPED" &&
        (sourceChanged || (!nearEnd && (transport == "STOPPED" || missingItem)))) {
        if (cancelledCurrent && !sourceChanged)
            throw TrackFault("queue cancelled item stopped after LMS flush");
        externalStop(sourceChanged ? "speaker-source-change" : "speaker-stop");
        return;
    }
    std::string mutation;
    if (unsolicited && expectedState == "PLAYING" && transport == "PAUSED_PLAYBACK") {

        expectedState = "PAUSED";
        mutation = "pause 1";
    } else if (unsolicited && expectedState == "PAUSED" && transport == "PLAYING" && !missingItem) {

        expectedState = "PLAYING";
        mutation = "pause 0";
    }
    if (missingItem && nearEnd && transport == "STOPPED")
        it = previousItem;
    if (it == items.end())
        return;
    if (transport == "PLAYING")
        waitingForPlay = false;
    if (waitingForPlay && transport == "STOPPED")
        return;
    if (!positionValid && !precise && !naturalEnd) {
        // A positionless UPnP transition leaves the previous clock in place.
        requestStatus();
        fastPoll();
        return;
    }
    const auto playback = precise && speakerTransitioning(precise->state) ? (state == "PAUSED" ? yeney::PlaybackState::Paused : yeney::PlaybackState::Playing)
                          : transport == "PLAYING"           ? yeney::PlaybackState::Playing
                          : transport == "PAUSED_PLAYBACK" ? yeney::PlaybackState::Paused
                                                           : yeney::PlaybackState::Stopped;
    if (playback == yeney::PlaybackState::Stopped && it != std::prev(items.end()))
        return;
    const auto elapsedMs = transport == "STOPPED" && nearEnd ? endEvidence * 1000 : reportedMs;
    const auto elapsed = elapsedMs / 1000;
    if (transport != "STOPPED")
        lastElapsed = elapsed;
    const auto frames = it->frames;
    const auto rate = it->item->descriptor().rate;
    if (transport == "PLAYING" && frames <= (elapsed + 3) * rate)
        fastPoll();
    if (it->stream != current) {
        fastPoll();
        if (current && transport == "PLAYING" && queue.queueItemProduced(current)) trackFaults.clear();
    }
    if (naturalEnd) {
        expectedState = "STOPPED";
        remoteNaturalSuccessor = estimatedEpisode && selectedIndex >= 0 &&
            size_t(selectedIndex+1) < playlist.entries.size();
    }
    const auto previous = current;
    const auto old = state;
    current = it->stream;
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        const auto owner = it->owner ? it->owner : it->stream;
        const auto index = rowOwner.sourceIndex(owner);
        const auto token = rowOwner.sourceToken(owner);
        if (index && token) rowOwner.acknowledgeObservation(*index, *token, current);
    }
    if (current != previous) windowDirty = true; // Refill lookahead on the audible boundary itself.
    state = playback == yeney::PlaybackState::Playing  ? "PLAYING"
            : playback == yeney::PlaybackState::Paused ? "PAUSED"
                                                       : "STOPPED";
    std::optional<int64_t> pauseTime;
    if (mutation == "pause 1" && pathReason == "remote-finite") {
        try {
            const auto snapshot = reader.read(host, mac);
            const auto owner = it->owner ? it->owner : it->stream;
            const auto before = queue.audibleElapsedMs(owner);
            if (before && snapshot.index < snapshot.entries.size() &&
                snapshot.entries[snapshot.index].url == it->sourceUrl)
                pauseTime = int64_t(snapshot.timeMs) + int64_t(it->segmentOffset * 1000 / rate + elapsedMs) - *before;
        } catch (const std::exception &) { logQueue("pause position baseline unavailable"); }
    }
    queue.speakerPosition(current, naturalEnd ? realEnd :
                          elapsedMs * it->item->descriptor().rate / 1000, playback);
    if (precise && !naturalEnd) {
        const auto owner = queue.audibleOccurrence();
        const auto baseline = queue.audibleElapsedMs(owner);
        if (baseline) {
            const auto offset = int64_t(*baseline) - int64_t(precise->positionMs);
            const auto item = precise->item;
            const auto fittedSource = selectedPosition.first;
            queue.positionClock(owner, [fittedSource, item, offset]() -> std::optional<uint32_t> {
                auto value = fittedSource->position(positionMonotonic());
                if (!value || value->item != item) return {};
                return uint32_t(std::clamp<int64_t>(int64_t(value->positionMs) + offset, 0, UINT32_MAX));
            });
        }
    }
    if (positionSourceChanged || websocketEvent || preciseSeek || rendererPositionConfirmed || current != previous || state != old)
        requestStatus();
    if (current != previous || state != old) {
        publish();
        logQueue("state=" + state + (state == "PLAYING" ? " audible occurrence=" : " occurrence=") +
                 std::to_string(current));
    }
    if (state == "PLAYING" && handoffAt != std::chrono::steady_clock::time_point{})
        handoffComplete();
    if (mutation == "pause 1" && precise && precise->state == "PAUSED" && pathReason == "local-source") {
        // LMS snapshots its playing clock when it handles pause. Wait for
        // the held coordinate to cross SlimProto before saving that clock.
        const auto target = queue.audibleElapsedMs(queue.audibleOccurrence());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        bool acknowledged = false;
        while (target && !pending() && std::chrono::steady_clock::now() < deadline) {
            requestStatus();
            std::string reply;
            if (!queue_audio::controlLms(host, mac, "time ?", 9090, &reply, 40)) break;
            const auto value = queue_audio::decodeLms(reply);
            try {
                const auto observed = std::stod(value.substr(value.find_last_of(' ') + 1)) * 1000;
                if (std::abs(observed - *target) <= 12) {
                    acknowledged = true;
                    break;
                }
            } catch (const std::exception &) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        logQueue(std::string("precise pause clock=") + (acknowledged ? "acknowledged" : "unconfirmed"));
        if (pending()) return;
        const auto latest = selectedPosition.first->position(positionMonotonic());
        if (latest && (latest->item != precise->item || speakerPlaying(latest->state))) {
            expectedState = "PLAYING";
            return;
        }
    }
    if (pauseTime) {
        // The CLI uses a different connection from SlimProto. Wait for the
        // server's read-only clock acknowledgement before it saves resumeTime.
        // This is a state barrier, not an audio delay or a CLI seek.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool acknowledged = false;
        int64_t observedTime = -1;
        do {
            try {
                auto snapshot = reader.read(host, mac);
                observedTime = snapshot.timeMs;
                acknowledged = std::abs(observedTime - *pauseTime) <= 1250;
                if (acknowledged) break;
            } catch (const std::exception &) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
        logQueue(std::string("pause position ") + (acknowledged ? "acknowledged" : "unconfirmed") +
                 " expected_lms_ms=" + std::to_string(*pauseTime) + " observed_lms_ms=" + std::to_string(observedTime));
    }
    if (pauseTime) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!commands.empty()) return; // New LMS control wins over an app echo.
    }
    if (!mutation.empty())
        speakerCommand(mutation, mutation == "pause 1" ? "speaker-pause" : "speaker-resume");
}
} // namespace bridge
