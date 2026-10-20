// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <string>

namespace bridge {
// Inputs are LMS fields after exactly one CLI decode. Never decode artwork again.
inline bool remoteArtworkTrack(const std::string &id, const std::string &remote, const std::string &url) {
    return remote == "1" || (!id.empty() && id.front() == '-') ||
           (remote.empty() && url.find(':') != std::string::npos && url.compare(0, 5, "file:"));
}
inline std::string lmsArtwork(const std::string &host, const std::string &id, const std::string &artwork,
                              bool remote) {
    const std::string base = "http://" + host + ":9000";
    if (!remote) {
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
            return {};
        return base + "/music/" + id + "/cover.jpg";
    }
    if (artwork.empty())
        return {};
    if (artwork.compare(0, 7, "http://") == 0 || artwork.compare(0, 8, "https://") == 0) {
        // RFC 3986 unreserved bytes only: the complete URL is one path component.
        const char *hex = "0123456789ABCDEF";
        std::string encoded;
        for (unsigned char c : artwork) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                c == '.' || c == '_' || c == '~')
                encoded += c;
            else {
                encoded += '%';
                encoded += hex[c >> 4];
                encoded += hex[c & 15];
            }
        }
        return base + "/imageproxy/" + encoded + "/image.jpg";
    }
    const auto first = artwork.find_first_not_of('/');
    return base + "/" + (first == std::string::npos ? std::string{} : artwork.substr(first));
}
} // namespace bridge
