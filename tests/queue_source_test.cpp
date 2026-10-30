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
    std::string reason;
    auto owner=queue_audio::splitPodcastTitle("Lavrov dreigt met verdere escalatie by BNR Nieuwsradio from Boekestijn & De Wijk",
        "Lavrov dreigt met verdere escalatie","Boekestijn & De Wijk | Plus",reason);
    assert(owner && owner->podcast=="Boekestijn & De Wijk" && owner->publisher=="BNR Nieuwsradio");
    auto america=queue_audio::splitPodcastTitle("Trump vestigt zijn hoop op het linkse Maine",
        "Trump vestigt zijn hoop op het linkse Maine","Amerika in 15 minuten",reason);
    assert(america && america->podcast=="Amerika in 15 minuten" && america->publisher.empty());
    auto delimiters=queue_audio::splitPodcastTitle("Episode by House by Writer from City from Coast",
        "Episode","City from Coast | Plus",reason);
    assert(delimiters && delimiters->publisher=="House by Writer" && delimiters->podcast=="City from Coast");
    puts("PASS owner title/name fixtures: suffix decoration, plain America title, publisher by and podcast from");
    auto split=queue_audio::splitPodcastTitle("Episode by Publisher from Feed | Series from Feed",
        "Episode","Feed | Plus",reason);
    assert(!split && reason.find("ambiguous split")!=std::string::npos);
    assert(reason.find("publisher=Publisher, podcast=Feed | Series from Feed")!=std::string::npos);
    assert(reason.find("publisher=Publisher from Feed | Series, podcast=Feed")!=std::string::npos);
    puts("PASS ambiguous split diagnostic: both candidate publisher/podcast pairs retained without source URLs");
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
    auto ambiguous=queue_audio::PodcastMenu::parse("id:1 title:Episode id:2 title:Episode%20by%20Writer count:2");
    assert(!ambiguous.plainTitle("Episode by Writer by Publisher from Feed"));
    auto literal=queue_audio::PodcastMenu::parse("id:1 title:Story%20by%20Author%20from%20Place count:1");
    assert(literal.plainTitle("Story by Author from Place")=="Story by Author from Place");
    for(unsigned mask=0;mask<8;++mask) {
        auto settings=bridge::roomPodcastSettings();
        for(unsigned i=0;i<3;++i)settings[i].enabled=mask&(1<<i);
        for(bool local:{false,true}) {
            queue_audio::PlaylistEntry own;own.url=local?"file:///local.flac":"podcast://remote";
            own.title="LMS title";own.artist="Own artist";own.album="Own album";
            queue_audio::applyPodcastMetadata(own,{"Episode","Name from Coast by Writer","Publisher by House from City"},settings);
            assert(own.artist=="Own artist" && own.album=="Own album");
            assert(own.title==(!local && settings[0].enabled?"Episode":"LMS title"));
        }
    }
    puts("PASS podcast metadata: own LMS artist/album win for all eight settings; local metadata unchanged");
    track.url="file:///local.flac";assert(track.plainTitle()==track.title);
    track.liveEdge=0;assert(!track.finiteRemote());
    track.liveEdge=10;assert(!track.finiteRemote());
    track.liveEdge=-1;track.duration=0;assert(!track.finiteRemote());
    track.duration=std::numeric_limits<double>::infinity();assert(!track.finiteRemote());
    puts("PASS finite remote metadata: LMS duration and live_edge; live/repeating edge, zero and nonfinite duration retain stream");
    puts("PASS queue source classification: local file only; remote flag, negative id, non-file URL and unknown use stream");
}
