// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_source.h"
#include "queue_playlist.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main() {
    using bridge::remotePlaybackSource;
    assert(!remotePlaybackSource("123", "0", "file:///music/local.flac"));
    assert(!remotePlaybackSource("123", "", "file:///music/local.flac"));
    assert(remotePlaybackSource("123", "1", "file:///music/local.flac"));
    assert(remotePlaybackSource("-95857230630456", "0", "file:///temporary"));
    assert(remotePlaybackSource("123", "0", "https://service.invalid/music"));
    assert(remotePlaybackSource("", "", "podcast://https://podcast.invalid/episode"));
    assert(remotePlaybackSource("", "", ""));
    queue_audio::PlaylistReader reader;
    const auto parsed=reader.parse("playlist_cur_index:0 playlist_tracks:1 time:20 playlist%20index:0 id:-1 url:podcast%3A%2F%2Fhttps%3A%2F%2Ftest.invalid%2Fepisode duration:60 live_edge:-1");
    auto track=parsed.entries.at(0);
    assert(parsed.timeMs==20000 && track.duration==60 && track.finiteRemote());
    track.liveEdge=0;assert(!track.finiteRemote());
    track.liveEdge=10;assert(!track.finiteRemote());
    track.liveEdge=-1;track.duration=0;assert(!track.finiteRemote());
    track.duration=std::numeric_limits<double>::infinity();assert(!track.finiteRemote());
    puts("PASS finite remote metadata: LMS duration and live_edge; live/repeating edge, zero and nonfinite duration retain stream");
    puts("PASS queue source classification: local file only; remote flag, negative id, non-file URL and unknown use stream");
}
