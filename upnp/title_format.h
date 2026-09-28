#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
namespace upnp {
enum class TitleFormat { ArtistTitle, Title };
inline TitleFormat titleFormat() {
    static const TitleFormat format = [] {
        const char* value = std::getenv("SONOS_LMS_TITLE_FORMAT");
        auto selected = TitleFormat::ArtistTitle;
        if (value && !std::strcmp(value, "title")) selected = TitleFormat::Title;
        else if (value && std::strcmp(value, "artist-title"))
            printf("Warning: invalid SONOS_LMS_TITLE_FORMAT='%s'; using artist-title\n", value);
        printf("SONOS_LMS_TITLE_FORMAT=%s\n", selected == TitleFormat::Title ? "title" : "artist-title");
        return selected;
    }();
    return format;
}
inline std::string formatTitle(const std::string& title, const std::string& artist,
                               TitleFormat format = titleFormat()) {
    return format == TitleFormat::ArtistTitle && !artist.empty() ? artist + " - " + title : title;
}
}
