// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
namespace bridge {
void QueueRoom::Impl::externalStop(const std::string &reason) {
    expectedState = "STOPPED";
    resumeEchoUntil = 0;
    resumeFlush = false;
    deferredStop = {};

    externalReleased = true;
    stopAcknowledged = false;
    firstStream = queue.latestOccurrence() + 1;
    // No Stop/URI switch here: another service may already own transport.
    try {
        const auto live = coordinator.currentRoom();
        if (live.coordinator == room.name) release();
        else { roomStopped = true; started = waitingForPlay = false; current = 0; state = "STOPPED"; publish(); }
    } catch (const std::exception &e) {
        fault = "external-release: " + std::string(e.what());
        started = waitingForPlay = false;
        current = 0;
        state = "STOPPED";
        logQueue(fault);
        publish();
    }
    speakerCommand("stop", reason);
}
void QueueRoom::Impl::observe() {
    const auto live = speaker->speaker();
    if (live.coordinator != room.name && (!stoppedPlay || started || state == "PLAYING" || state == "PAUSED") && !externalReleased) {
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
    const auto p = actions.position().response;
    const auto info = coordinator.transport("GetTransportInfo", {{"InstanceID", "0"}}).response;
    const auto media = coordinator.transport("GetMediaInfo", {{"InstanceID", "0"}}).response;
    speakerChanges->report("poll", {{"TransportState", info.value("CurrentTransportState")},
                                    {"TransportStatus", info.value("CurrentTransportStatus")},
                                    {"CurrentTrack", p.value("Track")},
                                    {"CurrentTrackURI", p.value("TrackURI")},
                                    {"NumberOfTracks", media.value("NrTracks")},
                                    {"AVTransportURI", media.value("CurrentURI")}});
    const auto uri = p.value("TrackURI");
    const auto avUri = media.value("CurrentURI");
    if (media.child("CurrentURI"))
        sourceAway = !avUri.empty() && avUri != "x-rincon-queue:" + room.uuid + "#0";
    if (!uri.empty() && std::none_of(items.begin(), items.end(), [&](const auto &i) { return i.uri == uri; }))
        sourceAway = true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!commands.empty())
            return; // SlimProto arrived while SOAP was in flight.
    }
    const auto transport = info.value("CurrentTransportState");
    const auto reportedStatus = info.value("CurrentTransportStatus");
    if (reportedStatus != transportStatus) {
        transportStatus = reportedStatus;
        logQueue("TransportStatus=" + transportStatus + " uri=" + uri);
    }
    auto it = std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.uri == uri; });
    if (sourceAway) {
        if (now >= ownUntil)
            externalStop("speaker-source-change");
        return; // Neither an error nor the echo guard authorises stopping a foreign source.
    }
    if (!reportedStatus.empty() && reportedStatus != "OK")
        throw std::runtime_error("playback-fault action=Play uri=" + uri +
                                 " TransportStatus=" + reportedStatus);
    if (waitingForPlay && transport == "STOPPED" &&
        std::chrono::steady_clock::now() - playAt > std::chrono::seconds(3))
        throw std::runtime_error("playback-fault action=Play uri=" + uri +
                                 " TransportStatus=" + reportedStatus + " stopped-after-play");
    const auto previousItem =
        std::find_if(items.begin(), items.end(), [&](const auto &i) { return i.stream == current; });
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
    const bool positionValid = sscanf(p.value("RelTime").c_str(), "%u:%u:%u", &ph, &pm, &ps) == 3;
    const uint64_t reportedElapsed = positionValid ? uint64_t(ph * 3600 + pm * 60 + ps) : 0;
    const auto endEvidence = it == previousItem ? std::max(lastElapsed, reportedElapsed) : lastElapsed;
    const bool nearEnd =
        finalItem && previousItem->frames <= (endEvidence + 2) * previousItem->item->descriptor().rate;
    const auto source = media.value("CurrentURI");
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
    if (sscanf(p.value("RelTime").c_str(), "%u:%u:%u", &h, &m, &s) != 3)
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
    if (transport == "STOPPED" && nearEnd)
        expectedState = "STOPPED";
    const auto previous = current;
    const auto old = state;
    current = it->stream;
    state = playback == yeney::PlaybackState::Playing  ? "PLAYING"
            : playback == yeney::PlaybackState::Paused ? "PAUSED"
                                                       : "STOPPED";
    queue.speakerPosition(current, elapsed * it->item->descriptor().rate, playback);
    if (current != previous || state != old) {
        publish();
        logQueue("state=" + state + (state == "PLAYING" ? " audible occurrence=" : " occurrence=") +
                 std::to_string(current));
    }
    if (state == "PLAYING" && handoffAt != std::chrono::steady_clock::time_point{})
        handoffComplete();
    if (!mutation.empty())
        speakerCommand(mutation, mutation == "pause 1" ? "speaker-pause" : "speaker-resume");
}
} // namespace bridge
