// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_playlist.h"
#include "upnp/queue_metadata.h"
#include <iostream>
int main() {
    std::string wire;
    std::getline(std::cin, wire);
    queue_audio::PlaylistReader reader;
    const auto snapshot = reader.parse("playlist_tracks:1 playlist_cur_index:0 playlist%20index:0 " + wire);
    const auto &entry = snapshot.entries.at(0);
    upnp::QueueMetadata metadata;
    metadata.url = "http://127.0.0.1/music/queue.wav?session=test&item=1";
    metadata.session = "test";
    metadata.frames = metadata.rate = 44100;
    metadata.size = 44 + 6 * metadata.frames;
    metadata.title = entry.title;
    metadata.artist = entry.artist;
    metadata.album = entry.album;
    metadata.artwork = entry.artwork("127.0.0.1");
    std::cout << upnp::queueDidl(metadata, upnp::TitleFormat::ArtistTitle);
}
