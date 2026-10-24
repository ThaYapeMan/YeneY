// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace bridge {
inline std::vector<std::string> debugOptions() {
    const char *value = std::getenv("YENEY_DEBUG");
    std::vector<std::string> result;
    if (!value) return result;
    const std::string text(value);
    for (size_t at = 0; at < text.size();) {
        const auto end = text.find(',', at);
        const auto token = text.substr(at, end == std::string::npos ? end : end - at);
        if (!token.empty()) result.push_back(token);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return result;
}
inline bool debugOption(const char *option, const char *legacy, const char *enabled = "1") {
    for (const auto &token : debugOptions()) if (token == option) return true;
    const char *value = std::getenv(legacy);
    return value && std::string(value) == enabled;
}
inline void warnDebugSettings() {
    static const bool once = [] {
        const struct { const char *name, *option; } aliases[] = {
            {"YENEY_DEBUG_EVENTS", "events"}, {"YENEY_DEBUG_STREAM", "stream"},
            {"YENEY_POLL", "legacy-poll"}, {"YENEY_STOPPED_MEDIAINFO", "stopped-mediainfo"}};
        for (const auto &alias : aliases)
            if (std::getenv(alias.name))
                std::fprintf(stderr, "yeney: %s is deprecated; use YENEY_DEBUG=%s\n", alias.name, alias.option);
        for (const auto &token : debugOptions())
            if (token != "events" && token != "stream" && token != "legacy-poll" && token != "stopped-mediainfo")
                std::fprintf(stderr, "yeney: unknown YENEY_DEBUG option=%s; ignored\n", token.c_str());
        return true;
    }();
    (void)once;
}
}
