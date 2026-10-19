// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_lms_cli.h"
#include <map>
#include <sstream>
#include <stdexcept>
namespace queue_audio {
struct PlaylistEntry {
    uint64_t token = 0;
    std::string id, title, artist, album, url;
};
struct PlaylistSnapshot {
    uint64_t revision = 0;
    unsigned index = 0, timeMs = 0;
    std::vector<PlaylistEntry> entries;
};
// Controller-worker only. LMS IDs identify recordings, tokens identify visits.
// Duplicate multiplicity is matched in order. Ambiguous duplicate edits fail
// closed rather than selecting an arbitrary recording occurrence.
class PlaylistReader {
    PlaylistSnapshot previous;
    uint64_t nextToken = 0, revision = 0;

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
            if (key == "playlist_cur_index")
                out.index = value == "-1" ? 0 : std::stoul(value);
            else if (key == "playlist_tracks")
                count = std::stoul(value);
            else if (key == "playlist_timestamp")
                timestamp = value;
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
            }
        }
        if (count > 60000 || out.entries.size() != count || (!count && out.index) ||
            (count && out.index >= count))
            throw std::runtime_error("playlist-incomplete");
        std::map<std::string, std::vector<uint64_t>> old;
        std::map<std::string, unsigned> used, counts;
        for (auto &e : previous.entries)
            old[e.id].push_back(e.token);
        for (auto &e : out.entries) {
            if (e.id.empty())
                e.id = e.url;
            if (e.id.empty())
                throw std::runtime_error("playlist-identity-missing");
            ++counts[e.id];
        }
        bool same = out.entries.size() == previous.entries.size();
        for (size_t i = 0; same && i < out.entries.size(); ++i)
            same = out.entries[i].id == previous.entries[i].id;
        for (auto &e : out.entries) {
            auto &ids = old[e.id];
            if (!same && !ids.empty() && (ids.size() > 1 || counts[e.id] > 1))
                throw std::runtime_error("playlist-duplicate-edit-ambiguous");
            auto n = used[e.id]++;
            e.token = n < ids.size() ? ids[n] : ++nextToken;
        }
        out.revision = same && revision ? revision : ++revision;
        previous = out;
        return out;
    }
    PlaylistSnapshot read(const std::string &host, const std::string &mac, unsigned port = 9090) {
        std::string reply;
        if (!controlLms(host, mac, "status 0 60000 tags:alKu", port, &reply))
            throw std::runtime_error("playlist-cli-unavailable");
        return parse(reply);
    }
};
} // namespace queue_audio
