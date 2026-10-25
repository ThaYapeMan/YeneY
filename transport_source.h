// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <string>
namespace bridge {
inline std::string transportSourceClass(const std::string &uri) {
    if (uri.rfind("x-rincon-queue:", 0) == 0) return "queue";
    if (uri.rfind("x-rincon:", 0) == 0) return "other-group member";
    if (uri.rfind("x-sonos-vli:", 0) == 0 && uri.find("spotify:") != std::string::npos) return "spotify-connect";
    if (uri.rfind("x-rincon-stream:", 0) == 0 || uri.rfind("x-sonos-htastream:", 0) == 0) return "line-in";
    if (uri.rfind("x-sonos-", 0) == 0 || uri.rfind("http:", 0) == 0 || uri.rfind("https:", 0) == 0) return "streaming service";
    return "unknown";
}
// Never put foreign tokens, credentials or opaque IDs into speaker-change logs.
inline std::string transportSourceLog(const std::string &uri) {
    return uri.empty() ? "<empty>" : transportSourceClass(uri) + " (redacted)";
}
}
