#pragma once
#include <cstdio>
#include <cstring>
#include <string>

extern "C" char** environ;

// Compatibility diagnostics only: none of these settings is read for configuration.
inline void warnLegacyEnvironment() {
    static const struct { const char* oldName; const char* newName; } names[] = {
        {"SONOS_LMS_UPNP", "YENEY_UPNP"},
        {"SONOS_LMS_PAUSE", "YENEY_PAUSE"},
        {"SONOS_SQUEEZEBOX_PAUSE", "YENEY_PAUSE"},
        {"SONOS_LMS_TITLE_FORMAT", "YENEY_TITLE_FORMAT"},
        {"SONOS_LMS_STREAM_CONTENT", "YENEY_STREAM_CONTENT"},
        {"SONOS_LMS_AUDIO", "YENEY_AUDIO"},
        {"SONOS_LMS_YENEY_POLL", "YENEY_POLL"},
        {"SONOS_LMS_YENEY_STOPPED_MEDIAINFO", "YENEY_STOPPED_MEDIAINFO"},
    };
    for (char** entry = environ; *entry; ++entry) {
        const std::string name(*entry, std::strcspn(*entry, "="));
        if (name.rfind("SONOS_LMS_", 0) != 0 && name.rfind("SONOS_SQUEEZEBOX_", 0) != 0) continue;
        const char* successor = nullptr;
        for (const auto& setting : names) if (name == setting.oldName) successor = setting.newName;
        if (successor) std::fprintf(stderr, "yeney: %s is no longer read; rename it to %s\n", name.c_str(), successor);
        else std::fprintf(stderr, "yeney: %s is obsolete\n", name.c_str());
    }
}
