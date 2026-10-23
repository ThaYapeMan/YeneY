// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_source.h"
#include <cassert>
#include <cstdio>
int main() {
    using bridge::remotePlaybackSource;
    assert(!remotePlaybackSource("123", "0", "file:///music/local.flac"));
    assert(!remotePlaybackSource("123", "", "file:///music/local.flac"));
    assert(remotePlaybackSource("123", "1", "file:///music/local.flac"));
    assert(remotePlaybackSource("-95857230630456", "0", "file:///temporary"));
    assert(remotePlaybackSource("123", "0", "https://service.invalid/music"));
    assert(remotePlaybackSource("", "", "podcast://https://podcast.invalid/episode"));
    assert(remotePlaybackSource("", "", ""));
    puts("PASS queue source classification: local file only; remote flag, negative id, non-file URL and unknown use stream");
}
