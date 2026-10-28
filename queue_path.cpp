// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_room_internal.h"
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
    selectedNewPlay = stoppedPlay ||
        (state == "STOPPED" && (handoffAt == std::chrono::steady_clock::time_point{} || externalReleased || faulted));
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
        selectedLocal = !remotePlaybackSource(track.id, track.remote, track.url) || track.finiteRemote();
        if (selectedNewPlay) track.startOffsetMs = playlist.timeMs;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            selectedTrack = track;
        }
        selectionKnown = true;
        if (!selectedNewPlay && selectedLocal != paths.isRelay()) {
            paths.waitBoundary();
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
