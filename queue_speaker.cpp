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
    const auto now = std::chrono::steady_clock::now();
    const bool event = eventDirty->exchange(false);
    if (!event && now < nextPoll)
        return;
    nextPoll = now + std::chrono::milliseconds(now < fastUntil ? 100 : pollMs);
    const auto positionStarted = std::chrono::steady_clock::now();
    const auto p = actions.position().response;
    auto position = speakerChanges->poll(positionStarted,
        {{"CurrentTrack", p.value("Track")}, {"CurrentTrackURI", p.value("TrackURI")},
         {"RelativeTimePosition", p.value("RelTime")}});
    const auto transportStarted = std::chrono::steady_clock::now();
    const auto info = coordinator.transport("GetTransportInfo", {{"InstanceID", "0"}}).response;
    const auto transportFields = speakerChanges->poll(transportStarted,
        {{"TransportState", info.value("CurrentTransportState")},
         {"TransportStatus", info.value("CurrentTransportStatus")}});
    const auto mediaStarted = std::chrono::steady_clock::now();
    const auto media = coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response;
    const auto mediaFields = speakerChanges->poll(mediaStarted,
        {{"NumberOfTracks", media.value("NrTracks")}, {"AVTransportURI", media.value("CurrentURI")}});
    const auto uri = position.at("CurrentTrackURI");
    const auto avUri = mediaFields.at("AVTransportURI");
    if (media.child("CurrentURI")) {
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
    const auto transport = transportFields.at("TransportState");
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
    if (checked != items.end()) {
        try {
            (void)checked->item->contentLength();
        } catch (const queue_audio::ItemCancelled &) {
            cancelledCurrent = true;
        }
    }
    unsigned ph = 0, pm = 0, ps = 0;
    const bool positionValid = sscanf(position.at("RelativeTimePosition").c_str(), "%u:%u:%u", &ph, &pm, &ps) == 3;
    const uint64_t reportedElapsed = positionValid ? uint64_t(ph * 3600 + pm * 60 + ps) : 0;
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
    const auto source = avUri;
    const bool sourceChanged = media.child("CurrentURI") && source != "x-rincon-queue:" + room.uuid + "#0" &&
                               (!source.empty() || !nearEnd);
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
    unsigned h = 0, m = 0, s = 0;
    if (sscanf(position.at("RelativeTimePosition").c_str(), "%u:%u:%u", &h, &m, &s) != 3)
        return;
    const auto playback = transport == "PLAYING"           ? yeney::PlaybackState::Playing
                          : transport == "PAUSED_PLAYBACK" ? yeney::PlaybackState::Paused
                                                           : yeney::PlaybackState::Stopped;
    if (playback == yeney::PlaybackState::Stopped && it != std::prev(items.end()))
        return;
    const auto elapsed = transport == "STOPPED" && nearEnd ? endEvidence : uint64_t(h * 3600 + m * 60 + s);
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
    if (transport == "STOPPED" && nearEnd) {
        expectedState = "STOPPED";
        remoteNaturalSuccessor = estimatedEpisode && selectedIndex >= 0 &&
            size_t(selectedIndex+1) < playlist.entries.size();
    }
    const auto previous = current;
    const auto old = state;
    current = it->stream;
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
                pauseTime = int64_t(snapshot.timeMs) + int64_t(it->segmentOffset * 1000 / rate + elapsed * 1000) - *before;
        } catch (const std::exception &) { logQueue("pause position baseline unavailable"); }
    }
    queue.speakerPosition(current, transport == "STOPPED" && nearEnd && estimatedEpisode ? realEnd :
                          elapsed * it->item->descriptor().rate, playback);
    if (current != previous || state != old) {
        publish();
        logQueue("state=" + state + (state == "PLAYING" ? " audible occurrence=" : " occurrence=") +
                 std::to_string(current));
    }
    if (state == "PLAYING" && handoffAt != std::chrono::steady_clock::time_point{})
        handoffComplete();
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
