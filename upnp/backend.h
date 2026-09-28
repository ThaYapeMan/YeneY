#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace upnp {
enum class Backend { Own };
inline Backend backend() {
    static const Backend selected = [] {
        const char* value = std::getenv("SONOS_LMS_UPNP");
        const bool alias = value && std::strcmp(value, "own") == 0;
        if (value && !alias && std::strcmp(value, "yeney"))
            printf("SONOS_LMS_UPNP=%s is no longer supported; using YeneY\n", value);
        printf("UPnP layer: yeney%s\n", alias ? " (alias own)" : "");
        return Backend::Own;
    }();
    return selected;
}
}
