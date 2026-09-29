#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
namespace upnp {
enum class StreamContentMode { Structured, Plain, Off };
inline StreamContentMode streamContentMode() {
    static const StreamContentMode mode = [] {
        const char* value = std::getenv("YENEY_STREAM_CONTENT");
        auto selected = StreamContentMode::Structured;
        if (value && !std::strcmp(value, "plain")) selected = StreamContentMode::Plain;
        else if (value && !std::strcmp(value, "off")) selected = StreamContentMode::Off;
        else if (value && std::strcmp(value, "structured"))
            printf("Warning: invalid YENEY_STREAM_CONTENT='%s'; using structured\n", value);
        printf("YENEY_STREAM_CONTENT=%s\n", selected == StreamContentMode::Structured ? "structured" :
            selected == StreamContentMode::Plain ? "plain" : "off");
        return selected;
    }();
    return mode;
}
inline std::string streamContent(const std::string& title, const std::string& artist,
                                 const std::string& album, StreamContentMode mode) {
    if (mode == StreamContentMode::Off) return {};
    if (mode == StreamContentMode::Plain) return artist.empty() ? title : artist + " - " + title;
    std::string result = "TYPE=SNG";
    auto append = [&](const char* field, std::string value) {
        if (value.empty()) return;
        std::replace(value.begin(), value.end(), '|', '/');
        result += std::string("|") + field + " " + value;
    };
    append("TITLE", title); append("ARTIST", artist); append("ALBUM", album);
    return result;
}
}
