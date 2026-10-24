// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <mutex>
#include <string>
namespace bridge {
struct StreamDisplay {
    std::mutex mutex;
    bool active = false;
    std::string title, artist, album, duration;
};
inline StreamDisplay &streamDisplay() { static auto *v = new StreamDisplay; return *v; }
inline void displayStreamTrack(const std::string &title, const std::string &artist,
                               const std::string &album, const std::string &duration) {
    auto &v = streamDisplay(); std::lock_guard<std::mutex> lock(v.mutex);
    v.active = true; v.title = title; v.artist = artist; v.album = album; v.duration = duration;
}
}
