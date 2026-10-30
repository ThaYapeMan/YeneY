// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "podcast_title.h"
#include "upnp/queue_metadata.h"
#include <iostream>
int main(int argc,char **argv) {
    if(argc!=5)return 2;
    auto settings=bridge::podcastSettings("Study",argv[4]);
    if(std::string(argv[1])=="settings")return 0;
    std::string diagnostic;
    auto metadata=queue_audio::podcastMetadata(argv[1],argv[2],argv[3],&diagnostic);
    if(!metadata) { std::cerr<<diagnostic<<"\n";return 1; }
    queue_audio::PlaylistEntry track;track.title=argv[3];track.url="podcast://fixture";
    queue_audio::applyPodcastMetadata(track,*metadata,settings);
    upnp::QueueMetadata item;item.title=track.title;item.artist=track.artist;item.album=track.album;
    item.url="http://fixture.invalid/episode.wav";item.session="fixture";item.frames=44100*65;item.rate=44100;item.size=44+item.frames*6;
    std::cout<<upnp::queueDidl(item)<<"\n";
}
