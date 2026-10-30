// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
#include "podcast_title.h"
#include "podcast_settings.h"
namespace bridge {
std::optional<yeney::FlacSourceInfo> QueueRoom::Impl::sourceInfo(uint64_t occurrence,
                                                                 std::optional<yeney::FlacSourceInfo> info) {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        auto found = sourceTracks.find(occurrence);
        if (found == sourceTracks.end())
            return info;
        id = found->second.id;
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
            return info;
        if (info) {
            flacCounts[id] = *info;
            return info;
        }
        auto cached = flacCounts.find(id);
        if (cached != flacCounts.end())
            return cached->second;
    }
    // Metadata only, never PCM/audio from this request. Headerless native
    // seeks retain the original source's frame number, not a Range offset.
    auto result = upnp::httpRequest("GET", {host, "/music/" + id + "/download", 9000},
                                    {{"Range", "bytes=0-8191"}}, {}, 700);
    if ((result.status != 200 && result.status != 206) || !result.error.empty() || result.body.size() < 42)
        return std::nullopt;
    auto p = reinterpret_cast<const unsigned char *>(result.body.data());
    if (std::string(result.body.data(), 4) != "fLaC" || (p[4] & 127) || p[5] || p[6] || p[7] != 34)
        return std::nullopt;
    yeney::FlacSourceInfo fetched;
    fetched.fixedBlockSize = unsigned(p[10]) * 256 + p[11];
    fetched.totalSamples = p[21] & 15;
    for (unsigned n = 22; n < 26; ++n)
        fetched.totalSamples = (fetched.totalSamples << 8) | p[n];
    if (!fetched.totalSamples || fetched.fixedBlockSize < 16)
        return std::nullopt;
    {
        std::lock_guard<std::mutex> lock(sourceMutex);
        flacCounts[id] = fetched;
    }
    logQueue("native FLAC exact metadata track_id=" + id +
             " source_frames=" + std::to_string(fetched.totalSamples));
    return fetched;
}
void QueueRoom::Impl::relayEvents(bool enabled) {
    if (!enabled) {
        speaker->queueEvents({});
        return;
    }
    speaker->queueEvents([dirty = eventDirty, changes = speakerChanges](const upnp::GenaEvent &event) {
        if (event.service == upnp::Service::AVTransport) {
            changes->event(event);
            *dirty = true;
        }
        return true;
    });
}
void QueueRoom::Impl::received(const yeney::Command &c) {
    if (c.letter == 's') {
        if (milliseconds() < resumeEchoUntil && resumeFlush) {
            selectionDeferred = false;
            return;
        }
        if (!selectionDeferred.exchange(true)) {
            std::lock_guard<std::mutex> lock(mutex);
            requestedCommand = c;
            commands.push_back({'I', ++selectionRequest});
            changed.notify_all();
        }
    } else {
        if (c.letter == 'q') {
            std::lock_guard<std::mutex> lock(mutex);
            ++selectionRequest;
            paths.cancelSelection();
            selectionDeferred = false;
            if (!paths.isRelay()) {
                commands.push_back({'Q', 0});
                changed.notify_all();
            }
        }
        paths.command(c);
    }
}
void QueueRoom::Impl::beginSelection(uint64_t request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (request != selectionRequest)
        return;
    if (stoppedPlay && stopAcknowledged) roomStopped = false;
    selectedRequest = request;
    selectedCommand = requestedCommand;
    selecting = true;
    selectionKnown = false;
    // A handoff temporarily releases transport before its new renderer is
    // confirmed. A fast source may already deliver its successor then; that
    // remains a track boundary, rather than a fresh play of the current index.
    selectedNewPlay = !remoteNaturalSuccessor && (stoppedPlay ||
        (state == "STOPPED" && (handoffAt == std::chrono::steady_clock::time_point{} || externalReleased || faulted)));
    remoteNaturalSuccessor = false;
}
void QueueRoom::Impl::selectPath() {
    if (!selecting || selectedRequest != selectionRequest)
        return;
    if (!selectionKnown) {
        const auto live = coordinator.currentRoom();
        if (live.coordinator != room.name) {
            if (!roomStopped) {
                std::printf("yeney: room is group member room=%s coordinator=%s; not playing\n",
                            room.name.c_str(), live.coordinator.c_str());
                roomFault("group-member", true);
            }
            selecting = false;
            return;
        }
        if (externalReleased && ownership.active()) release();
        roomStopped = false;

        queue_audio::PlaylistEntry track;
        selectedIndex = -1;
        try {
            playlist = reader.read(host, mac);
            const auto index = playlist.index + (selectedNewPlay ? 0 : 1);
            if (index < playlist.entries.size()) {
                track = playlist.entries[index];
                selectedIndex = int(index);
            }
        } catch (const std::exception &e) {
            logQueue("source metadata unavailable; using stream: " + std::string(e.what()));
        }
        if (selectedRequest != selectionRequest) {
            selecting = false;
            return;
        }
        if (remotePlaybackSource(track.id, track.remote, track.url) && !track.url.empty()) {
            if (track.finiteRemote() && selectedIndex == int(playlist.index) &&
                std::isfinite(playlist.duration) && playlist.duration > track.duration) {
                logQueue("duration inconsistent entry_s=" + std::to_string(track.duration) +
                         " status_s=" + std::to_string(playlist.duration) + " decision=larger-status-bound");
                track.duration = playlist.duration;
            }
            if (track.finiteRemote()) {
                auto identity = track.url;
                const auto marker = identity.rfind("{from=");
                if (marker != std::string::npos && identity.back() == '}') identity.resize(marker);
                auto known = plainRemoteTitles.find(identity);
                if (known == plainRemoteTitles.end() || !known->second.complete) {
                    if (known != plainRemoteTitles.end()) logQueue("podcast metadata step=cache reason=cache hit without podcast fields; fresh lookup");
                    PodcastFields fields{roomPodcastSettings()[0].enabled ? track.plainTitle() : track.title,{}, {},
                        track.url.compare(0,10,"podcast://") != 0};
                    if (track.url.compare(0,10,"podcast://") == 0) {
                        try {
                            std::string diagnostic;
                            if (auto metadata = queue_audio::podcastMetadata(host,mac,track.title,&diagnostic)) {
                                fields={metadata->title,metadata->podcast,metadata->publisher,
                                    !metadata->podcast.empty() && !metadata->publisher.empty()};
                                logQueue("title source=podcast-menu");
                                if (!diagnostic.empty()) logQueue("podcast metadata step=" + diagnostic);
                            } else logQueue("podcast metadata step=" + diagnostic);
                        } catch (const std::exception &) { logQueue("podcast metadata step=lookup reason=exception"); }
                    }
                    plainRemoteTitles[identity]=std::move(fields);
                    known = plainRemoteTitles.find(identity);
                }
                if (roomPodcastSettings()[0].enabled) track.title = known->second.title;
                if (track.artist.empty() && roomPodcastSettings()[1].enabled) track.artist = known->second.artist;
                if (track.album.empty() && roomPodcastSettings()[2].enabled) track.album = known->second.album;
            }
        }
        if (selectedRequest != selectionRequest) { selecting = false;return; }
        selectedLocal = !remotePlaybackSource(track.id, track.remote, track.url) || track.finiteRemote();
        // Retain the status clock for diagnostics only. Remote promises never
        // subtract this possibly previous-run value.
        if (selectedNewPlay) track.startOffsetMs = playlist.timeMs;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            selectedTrack = track;
        }
        selectionKnown = true;
        if (!selectedNewPlay && selectedLocal != paths.isRelay()) {
            paths.waitBoundary(state == "STOPPED" && started && expectedState == "STOPPED");
            logQueue("path-pending path=" + std::string(selectedLocal ? "relay" : "stream") +
                     " waiting=audible-end");
        }
    }
    const bool switching = selectedLocal != paths.isRelay();
    if (switching && !selectedNewPlay && !paths.atBoundary())
        return;
    if (switching) {
        if (!selectedNewPlay)
            handoffAt = std::chrono::steady_clock::now();
        if (paths.isRelay()) {
            release(ownership.active());
            queue.flush();
            queue.waitIdle();
        } else {
            yeney_queue_stream_handoff();
            speaker->stop();
        }
        relayEvents(selectedLocal);
    }
    bool continues = false;
    if (selectedIndex >= 0 && size_t(selectedIndex + 1) < playlist.entries.size()) {
        const auto &next = playlist.entries[size_t(selectedIndex + 1)];
        continues = (!remotePlaybackSource(next.id, next.remote, next.url) || next.finiteRemote()) == selectedLocal;
    }
    paths.mayContinue(continues);
    metadataIndex = !selectedLocal && !selectedNewPlay ? selectedIndex : -1;
    pathReason = selectedLocal ? (remotePlaybackSource(selectedTrack.id, selectedTrack.remote, selectedTrack.url)
                                     ? "remote-finite" : "local-source") : "remote-source";
    logQueue("path=" + std::string(selectedLocal ? "relay" : "stream") + " reason=" + pathReason +
             " boundary=" + (selectedNewPlay ? "new-play" : "track-end"));
    if (selectedLocal)
        command('s', queue.latestOccurrence() + 1);
    // Core resets only at a class boundary, without HELO or an LMS command.
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (selectedRequest != selectionRequest) {
            selecting = false;
            return;
        }
        paths.select(selectedLocal, switching, selectedCommand);
        selectionDeferred = false;
    }
    stoppedPlay = false;
    selecting = selectionKnown = false;
    publish();
}
void QueueRoom::Impl::handoffComplete() {
    const auto gap =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - handoffAt)
            .count();
    logQueue("path-handoff path=" + std::string(paths.isRelay() ? "relay" : "stream") +
             " gap_ms=" + std::to_string(gap));
    handoffAt = {};
}
} // namespace bridge
