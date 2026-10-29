// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_source.h"
#include "queue_playlist.h"
#include "podcast_title.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main(int argc,char **argv) {
    if(argc==4) {
        auto title=queue_audio::podcastPlainTitle(argv[1],argv[2],argv[3]);
        if(!title)return 1;
        std::printf("%s\n",title->c_str());return 0;
    }
    using bridge::remotePlaybackSource;
    assert(!remotePlaybackSource("123", "0", "file:///music/local.flac"));
    assert(!remotePlaybackSource("123", "", "file:///music/local.flac"));
    assert(remotePlaybackSource("123", "1", "file:///music/local.flac"));
    assert(remotePlaybackSource("-95857230630456", "0", "file:///temporary"));
    assert(remotePlaybackSource("123", "0", "https://service.invalid/music"));
    assert(remotePlaybackSource("", "", "podcast://https://podcast.invalid/episode"));
    assert(remotePlaybackSource("", "", ""));
    queue_audio::PlaylistReader reader;
    const auto parsed=reader.parse("playlist_cur_index:0 playlist_tracks:1 time:20 duration:90 playlist%20index:0 id:-1 url:podcast%3A%2F%2Fhttps%3A%2F%2Ftest.invalid%2Fepisode duration:60 live_edge:-1");
    auto track=parsed.entries.at(0);
    assert(parsed.duration==90 && parsed.timeMs==20000 && track.duration==60 && track.finiteRemote());
    track.title="Episode by BNR Nieuwsradio from Boekestijn & De Wijk";
    track.artist="BNR Nieuwsradio";track.album="Boekestijn & De Wijk";
    assert(track.plainTitle()=="Episode");
    auto menu=queue_audio::PodcastMenu::parse("id:4.0 type:link title:Lavrov%20dreigt%20met%20verdere%20escalatie isaudio:1 count:1703");
    assert(menu.count==1703 && menu.plainTitle("Lavrov dreigt met verdere escalatie by BNR Nieuwsradio from Boekestijn & De Wijk")=="Lavrov dreigt met verdere escalatie");
    auto literal=queue_audio::PodcastMenu::parse("id:1 title:Story%20by%20Author%20from%20Place count:1");
    assert(literal.plainTitle("Story by Author from Place")=="Story by Author from Place");
    track.url="file:///local.flac";assert(track.plainTitle()==track.title);
    track.liveEdge=0;assert(!track.finiteRemote());
    track.liveEdge=10;assert(!track.finiteRemote());
    track.liveEdge=-1;track.duration=0;assert(!track.finiteRemote());
    track.duration=std::numeric_limits<double>::infinity();assert(!track.finiteRemote());
    puts("PASS finite remote metadata: LMS duration and live_edge; live/repeating edge, zero and nonfinite duration retain stream");
    puts("PASS queue source classification: local file only; remote flag, negative id, non-file URL and unknown use stream");
}
