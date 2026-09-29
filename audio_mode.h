#pragma once
#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#include <cstring>
enum class AudioMode { Modern, Legacy };
inline AudioMode audioMode() {
    static const AudioMode mode = [] {
        const char* value = std::getenv("YENEY_AUDIO");
        const bool legacy = value && !std::strcmp(value, "16/44");
        if (value && !legacy && std::strcmp(value, "24/48"))
            printf("Warning: invalid YENEY_AUDIO='%s'; using 24/48\n", value);
        printf("YENEY_AUDIO=%s\n", legacy ? "16/44" : "24/48");
        return legacy ? AudioMode::Legacy : AudioMode::Modern;
    }();
    return mode;
}
extern "C" {
#endif
int sonos_audio_legacy(void);
void sonos_output_new_track(int continuous);
void set_squeezebox_audio_rate(unsigned stream, unsigned rate);
#ifdef __cplusplus
}
#endif
