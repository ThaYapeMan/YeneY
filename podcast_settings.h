// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <array>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
namespace bridge {
struct PodcastSetting { const char *key; bool enabled; std::string source = "default"; };
using PodcastSettings = std::array<PodcastSetting,3>;
inline PodcastSettings &roomPodcastSettings() {
    static PodcastSettings settings{{{"YENEY_PODCAST_TITLE_PLAIN",true},
        {"YENEY_PODCAST_ARTIST_SHOW",true},{"YENEY_PODCAST_ALBUM_PUBLISHER",false}}};
    return settings;
}
inline PodcastSettings podcastSettings(const std::string &room, const char *path) {
    PodcastSettings result{{{"YENEY_PODCAST_TITLE_PLAIN",true},
        {"YENEY_PODCAST_ARTIST_SHOW",true},{"YENEY_PODCAST_ALBUM_PUBLISHER",false}}};
    std::array<std::optional<std::string>,3> global, local;
    auto trim=[](std::string s) { auto first=s.find_first_not_of(" \t\r");
        return first==std::string::npos?std::string{}:s.substr(first,s.find_last_not_of(" \t\r")-first+1); };
    std::ifstream input(path);std::string line;
    while(std::getline(input,line)) {
        auto eq=line.find('=');if(eq==std::string::npos)continue;
        auto key=trim(line.substr(0,eq)),value=trim(line.substr(eq+1));
        for(size_t i=0;i<result.size();++i) {
            if(key==result[i].key && !global[i])global[i]=value;
            if(key=="room."+room+"."+result[i].key && !local[i])local[i]=value;
        }
    }
    for(size_t i=0;i<result.size();++i) {
        auto &setting=result[i];const char *env=std::getenv(setting.key);
        const auto value=env?std::string(env):local[i]?*local[i]:global[i]?*global[i]:setting.enabled?"yes":"no";
        setting.source=env?"environment":local[i]?"room-config":global[i]?"global-config":"default";
        if(value=="yes" || value=="no")setting.enabled=value=="yes";
        else std::printf("yeney: invalid %s; using %s\n",setting.key,setting.enabled?"yes":"no");
        std::printf("yeney: %s=%s source=%s\n",setting.key,setting.enabled?"yes":"no",setting.source.c_str());
    }
    return result;
}
} // namespace bridge
