// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_lms_cli.h"
#include "queue_playlist.h"
#include "podcast_settings.h"
#include <chrono>
#include <cstdio>
#include <optional>
#include <sstream>
#include <vector>
namespace queue_audio {
struct PodcastMetadata { std::string title, podcast, publisher; };
inline void applyPodcastMetadata(PlaylistEntry &track, const PodcastMetadata &metadata,
                                 const bridge::PodcastSettings &settings) {
    if (track.url.compare(0,10,"podcast://") != 0) return;
    if (settings[0].enabled) track.title=metadata.title;
    if (track.artist.empty() && settings[1].enabled) track.artist=metadata.podcast;
    if (track.album.empty() && settings[2].enabled) track.album=metadata.publisher;
}
struct PodcastMenu {
    struct Row { std::string id, name, title; };
    std::vector<Row> rows;
    unsigned count = 0;
    static PodcastMenu parse(const std::string &wire) {
        PodcastMenu menu;std::istringstream input(wire);std::string field;
        while(input>>field) {
            field=decodeLms(field);auto colon=field.find(':');
            if(colon==std::string::npos)continue;
            auto key=field.substr(0,colon),value=field.substr(colon+1);
            if(key=="id")menu.rows.push_back({value,{},{}});
            else if(key=="count")menu.count=unsigned(std::stoul(value));
            else if(!menu.rows.empty()) {
                if(key=="name")menu.rows.back().name=value;
                if(key=="title")menu.rows.back().title=value;
            }
        }
        return menu;
    }
    std::optional<std::string> plainTitle(const std::string &formatted) const {
        std::optional<std::string> candidate;
        for(const auto &row:rows) {
            const auto &title=row.title.empty()?row.name:row.title;
            if(title.empty())continue;
            if(title==formatted)return title; // a literal title containing by/from stays intact
            if(formatted.compare(0,title.size()+4,title+" by ")==0) {
                if(candidate && *candidate!=title)return std::nullopt;
                candidate=title;
            }
        }
        return candidate;
    }
};
// Only native Podcast's stored player-formatted TITLE needs this lookup. Query
// the matching feed, not audio/download URLs; retain the CLI's single decoding.
inline std::optional<PodcastMetadata> podcastMetadata(const std::string &host,const std::string &mac,
                                                   const std::string &formatted) {
    const auto by=formatted.find(" by "),from=formatted.rfind(" from ");
    if(by==std::string::npos || from==std::string::npos || from<=by)return std::nullopt;

    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    auto query=[&](const std::string &command)->std::optional<PodcastMenu> {
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(left<=0)return std::nullopt;
        std::string reply;
        if(!controlLms(host,mac,command,9090,&reply,int(left)))return std::nullopt;
        return PodcastMenu::parse(reply);
    };
    auto top=query("podcasts items 0 100 want_url:1");
    if(!top)return std::nullopt;
    const PodcastMenu::Row *matched=nullptr;
    std::string matchedName;
    for(const auto &feed:top->rows) {
        auto name=feed.name.empty()?feed.title:feed.name;
        auto decoration=name.rfind(" |");
        if(decoration!=std::string::npos)name.resize(decoration);
        const auto suffix=" from "+name;
        if(name.empty() || formatted.size()<suffix.size() ||
           formatted.compare(formatted.size()-suffix.size(),suffix.size(),suffix))continue;
        if(matched) { std::printf("yeney: podcast metadata ambiguous feed suffix; fields omitted\n");return std::nullopt; }
        matched=&feed;matchedName=name;
    }
    if(matched) {
        const auto &feed=*matched;
        const auto &feedName=matchedName;
        const auto suffix=" from "+feedName;
        unsigned count=100;
        for(unsigned first=0;first<count && first<60000;first+=100) {
            if(feed.id.find_first_not_of("0123456789.")!=std::string::npos)break;
            auto page=query("podcasts items "+std::to_string(first)+" 100 want_url:1 item_id:"+feed.id);
            if(!page)return std::nullopt;
            if(auto plain=page->plainTitle(formatted)) {
                PodcastMetadata metadata{*plain,feedName,{}};
                const auto prefix=*plain+" by ";
                if(formatted.compare(0,prefix.size(),prefix)==0 &&
                   formatted.size()>=prefix.size()+suffix.size())
                    metadata.publisher=formatted.substr(prefix.size(),formatted.size()-prefix.size()-suffix.size());
                return metadata;
            }
            count=page->count;
        }
    }
    return std::nullopt;
}
inline std::optional<std::string> podcastPlainTitle(const std::string &host,const std::string &mac,
                                                   const std::string &formatted) {
    auto metadata=podcastMetadata(host,mac,formatted);
    return metadata?std::optional<std::string>(metadata->title):std::nullopt;
}
} // namespace queue_audio
