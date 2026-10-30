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
    struct Row { std::string id, name, title; bool feed = false; };
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
                if(key=="url")menu.rows.back().feed=true;
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
// Compare menu decorations without changing the name extracted from LMS TITLE.
inline std::string podcastFeedBase(std::string name) {
    auto decoration=name.find(" | ");
    if(decoration!=std::string::npos)name.resize(decoration);
    return name;
}
inline bool podcastFeedMatches(const std::string &menu,const std::string &name) {
    return !name.empty() && podcastFeedBase(menu)==podcastFeedBase(name);
}
inline std::optional<PodcastMetadata> splitPodcastTitle(const std::string &formatted,
        const std::string &plain,const std::string &feed,std::string &reason) {
    if(formatted==plain)return PodcastMetadata{plain,podcastFeedBase(feed),{}};
    const auto prefix=plain+" by ";
    if(formatted.compare(0,prefix.size(),prefix)!=0) {
        reason="episode reason=no episode match";return std::nullopt;
    }
    std::vector<PodcastMetadata> candidates;
    for(auto pos=formatted.find(" from ",prefix.size());pos!=std::string::npos;
        pos=formatted.find(" from ",pos+1)) {
        auto publisher=formatted.substr(prefix.size(),pos-prefix.size());
        auto podcast=formatted.substr(pos+6);
        if(!publisher.empty() && podcastFeedMatches(feed,podcast))
            candidates.push_back({plain,podcast,publisher});
    }
    if(candidates.size()==1)return candidates.front();
    reason=candidates.empty()?"split reason=no feed match":"split reason=ambiguous split";
    // Names are the only diagnostic payload: never source URLs or tokens.
    for(const auto &candidate:candidates)
        reason+=" [publisher="+candidate.publisher+", podcast="+candidate.podcast+"]";
    return std::nullopt;
}
inline std::optional<PodcastMetadata> podcastMetadata(const std::string &host,const std::string &mac,
        const std::string &formatted,std::string *diagnostic=nullptr) {
    auto fail=[&](const std::string &reason)->std::optional<PodcastMetadata> {
        if(diagnostic)*diagnostic=reason;
        return std::nullopt;
    };
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    std::string failure;
    auto query=[&](const std::string &command)->std::optional<PodcastMenu> {
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(left<=0){failure="lookup reason=lookup timeout";return std::nullopt;}
        std::string reply;
        if(!controlLms(host,mac,command,9090,&reply,int(left))) {
            failure=std::chrono::steady_clock::now()>=deadline ? "lookup reason=lookup timeout":"lookup reason=CLI query failed";
            return std::nullopt;
        }
        return PodcastMenu::parse(reply);
    };
    auto top=query("podcasts items 0 100 want_url:1");
    if(!top)return fail(failure);
    std::optional<PodcastMetadata> result;
    struct FeedPage { PodcastMenu::Row feed; unsigned count=100; };
    std::vector<FeedPage> feeds;
    const bool formattedShape=formatted.find(" by ")!=std::string::npos && formatted.find(" from ")!=std::string::npos;
    for(const auto &feed:top->rows) {
        const auto name=feed.name.empty()?feed.title:feed.name;
        if(name.empty() || feed.id.empty() || feed.id.find_first_not_of("0123456789.")!=std::string::npos)continue;
        bool plausible=!formattedShape && feed.feed;
        for(auto pos=formatted.find(" from ");pos!=std::string::npos;pos=formatted.find(" from ",pos+1))
            plausible=plausible || podcastFeedMatches(name,formatted.substr(pos+6));
        if(plausible)feeds.push_back({feed,100});
    }
    // Visit the first page of every plausible feed before older pages. A large
    // unrelated subscription must not exhaust the budget before a recent match.
    for(unsigned first=0;first<60000;first+=100) {
        bool pending=false;
        for(auto &candidate:feeds) {
            if(first>=candidate.count)continue;
            pending=true;
            const auto &feed=candidate.feed;
            const auto name=feed.name.empty()?feed.title:feed.name;
            auto page=query("podcasts items "+std::to_string(first)+" 100 want_url:1 item_id:"+feed.id);
            if(!page)return fail(failure);
            candidate.count=page->count;
            if(auto plain=page->plainTitle(formatted)) {
                std::string reason;
                auto metadata=splitPodcastTitle(formatted,*plain,name,reason);
                if(!metadata)return fail(reason);
                if(result)return fail("episode reason=ambiguous episode match across feeds");
                result=metadata;
            }
        }
        if(result || !pending)break;
    }
    if(result) {
        if(diagnostic && result->publisher.empty())*diagnostic="split reason=plain LMS title has no publisher";
        return result;
    }
    return fail(!feeds.empty()?"episode reason=no episode match":"feed reason=no feed match");
}
inline std::optional<std::string> podcastPlainTitle(const std::string &host,const std::string &mac,
                                                   const std::string &formatted) {
    auto metadata=podcastMetadata(host,mac,formatted);
    return metadata?std::optional<std::string>(metadata->title):std::nullopt;
}
} // namespace queue_audio
