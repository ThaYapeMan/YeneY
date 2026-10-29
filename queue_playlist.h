// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "lms_artwork.h"
#include "queue_lms_cli.h"
#include <map>
#include <cmath>
#include <sstream>
#include <stdexcept>
namespace queue_audio {
struct PlaylistEntry {
    std::string id, title, artist, album, url;
    std::string artworkUrl, remote;
    double duration = 0, liveEdge = -1;
    unsigned startOffsetMs = 0;
    std::string plainTitle() const {
        if (url.compare(0, 10, "podcast://")) return title;
        // Native Podcast scanUrl can store getCurrentTitle's player formatting
        // back into TITLE. Remove only the exact suffix made from these tags.
        const auto suffix = (!artist.empty() ? " by " + artist : std::string{}) +
                            (!album.empty() ? " from " + album : std::string{});
        if (!suffix.empty() && title.size() >= suffix.size() &&
            title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0)
            return title.substr(0, title.size() - suffix.size());
        return title;
    }
    bool finiteRemote() const { return std::isfinite(duration) && duration > 0 && liveEdge < 0; }
    std::string artwork(const std::string &host) const {
        return bridge::lmsArtwork(host, id, artworkUrl, bridge::remoteArtworkTrack(id, remote, url));
    }
};
struct PlaylistSnapshot {
    unsigned index = 0, timeMs = 0;
    std::string mode;
    double duration = 0;
    std::vector<PlaylistEntry> entries;
};
// Read-only metadata snapshot. LMS alone determines stream order; the relay
// has no occurrence matching, playlist edit inference or mirroring policy.
class PlaylistReader {
public:
    PlaylistSnapshot parse(const std::string &wire) {
        PlaylistSnapshot out;
        std::istringstream input(wire);
        std::string field, timestamp;
        PlaylistEntry *entry = nullptr;
        unsigned count = 0;
        while (input >> field) {
            field = decodeLms(field);
            auto colon = field.find(':');
            if (colon == std::string::npos)
                continue;
            auto key = field.substr(0, colon), value = field.substr(colon + 1);
            if (key == "mode" && !entry)
                out.mode = value;
            else if (key == "playlist_cur_index")
                out.index = value == "-1" ? 0 : std::stoul(value);
            else if (key == "playlist_tracks")
                count = std::stoul(value);
            else if (key == "playlist_timestamp")
                timestamp = value;
            else if (key == "duration" && !entry)
                out.duration = std::stod(value);
            else if (key == "time")
                out.timeMs = unsigned(std::stod(value) * 1000);
            else if (key == "playlist index" || key == "index") {
                if (std::stoul(value) != out.entries.size())
                    throw std::runtime_error("playlist-incomplete");
                out.entries.emplace_back();
                entry = &out.entries.back();
            } else if (entry) {
                if (key == "id")
                    entry->id = value;
                else if (key == "title")
                    entry->title = value;
                else if (key == "artist")
                    entry->artist = value;
                else if (key == "album")
                    entry->album = value;
                else if (key == "url")
                    entry->url = value;
                else if (key == "artwork_url")
                    entry->artworkUrl = value;
                else if (key == "duration")
                    entry->duration = std::stod(value);
                else if (key == "live_edge")
                    entry->liveEdge = std::stod(value);
                else if (key == "remote")
                    entry->remote = value;
            }
        }
        if (count > 60000 || out.entries.size() != count || (!count && out.index) ||
            (count && out.index >= count))
            throw std::runtime_error("playlist-incomplete");
        return out;
    }
    PlaylistSnapshot read(const std::string &host, const std::string &mac, unsigned port = 9090) {
        std::string reply;
        if (!controlLms(host, mac, "status 0 60000 tags:alKudV", port, &reply))
            throw std::runtime_error("playlist-cli-unavailable");
        return parse(reply);
    }
};
} // namespace queue_audio
