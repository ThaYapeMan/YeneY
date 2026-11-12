// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_playlist.h"
#include <algorithm>
#include <deque>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace queue_audio {
// Ordinals are one based, LMS indices zero based. Continuations retain their
// parent's token; they do not consume another history or look-ahead slot.
class QueueWindow {
public:
    static constexpr unsigned historyCount = 2;
    static constexpr unsigned upcomingCount = 3;
    static constexpr unsigned settleMs = 250;
    using Token = uint64_t;
    struct Slot {
        Token token;
        unsigned index;
        PlaylistEntry track;
    };
    struct Policy { std::string revision; unsigned shuffle = 0, repeat = 0; };
    struct Segment { uint64_t occurrence = 0, offsetFrames = 0; };
    using Segments = std::map<Token, std::vector<Segment>>;
    struct Position { Slot slot; Segment segment; unsigned segmentIndex; };
    struct Edit {
        enum Kind { Remove, Insert } kind;
        unsigned ordinal;
        Token token;
    };
private:
    unsigned rowCap = 500;
    Token serial = 0;
    Policy observedPolicy;
    std::vector<Slot> playlist;
    static std::string identity(const PlaylistEntry &entry) {
        // Length prefix avoids delimiter collisions in arbitrary CLI fields.
        return std::to_string(entry.id.size()) + ":" + entry.id + entry.url;
    }
public:
    explicit QueueWindow(unsigned cap = 500) : rowCap(cap) {
        if (!cap || cap > 60000) throw std::invalid_argument("invalid queue row cap");
    }
    unsigned cap() const { return rowCap; }
    static bool eligible(const PlaylistEntry &entry) {
        return !entry.url.empty() && entry.finiteRemote();
    }
    // LMS status order is authoritative, including its shuffled playback list.
    // Match duplicate identities in occurrence order. No title-based matching.
    void reconcile(const PlaylistSnapshot &snapshot) {
        if (snapshot.shuffle > 2 || snapshot.repeat > 2)
            throw std::invalid_argument("window playlist policy invalid");
        if ((!snapshot.entries.empty() && snapshot.index >= snapshot.entries.size()) ||
            (snapshot.entries.empty() && snapshot.index))
            throw std::invalid_argument("window playlist index out of bounds");
        std::map<std::string, std::deque<Token>> previous;
        for (const auto &slot : playlist) previous[identity(slot.track)].push_back(slot.token);
        std::vector<Slot> next;
        next.reserve(snapshot.entries.size());
        for (unsigned index = 0; index < snapshot.entries.size(); ++index) {
            const auto &entry = snapshot.entries[index];
            auto &matches = previous[identity(entry)];
            const auto token = matches.empty() ? ++serial : matches.front();
            if (!matches.empty()) matches.pop_front();
            next.push_back({token, index, entry});
        }
        playlist.swap(next);
        observedPolicy = {snapshot.revision, snapshot.shuffle, snapshot.repeat};
    }
    const Policy &policy() const { return observedPolicy; }
    std::optional<Slot> atIndex(unsigned index) const {
        return index < playlist.size() ? std::optional<Slot>(playlist[index]) : std::nullopt;
    }
    std::vector<Slot> around(unsigned index) const {
        if (playlist.empty()) return {};
        if (index >= playlist.size()) throw std::out_of_range("window index");
        if (!eligible(playlist[index].track)) return {};
        // A continuous source divides finite queue ownership. Inside a finite
        // playlist the complete capped prefix is independent of the playhead.
        unsigned first = 0, last = std::min<unsigned>(playlist.size(), rowCap);
        if (index >= rowCap) {
            first = index > historyCount ? index - historyCount : 0;
            last = std::min<unsigned>(playlist.size(), index + upcomingCount + 1);
        }
        while (first < index && !eligible(playlist[first].track)) ++first;
        for (unsigned n = first; n < last; ++n) {
            if (!eligible(playlist[n].track)) { last = n; break; }
        }
        return {playlist.begin() + first, playlist.begin() + last};
    }
    std::optional<Slot> find(Token token) const {
        const auto it = std::find_if(playlist.begin(), playlist.end(),
            [token](const auto &slot) { return slot.token == token; });
        return it == playlist.end() ? std::nullopt : std::optional<Slot>(*it);
    }
    // Expand only the speaker ordinal space, never the parent/history budget.
    // A lazy parent without a produced segment still occupies one real entry.
    static Position atOrdinal(const std::vector<Slot> &window, const Segments &segments, unsigned ordinal) {
        if (!ordinal) throw std::out_of_range("window ordinal");
        for (const auto &slot : window) {
            const auto found = segments.find(slot.token);
            const auto count = found == segments.end() ? 1 : found->second.size();
            if (!count) throw std::invalid_argument("window parent has no segments");
            if (ordinal <= count) return {slot, found == segments.end() ? Segment{} : found->second[ordinal - 1], ordinal - 1};
            ordinal -= count;
        }
        throw std::out_of_range("window ordinal");
    }
    static unsigned ordinalOf(const std::vector<Slot> &window, const Segments &segments,
                              Token token, unsigned segmentIndex = 0) {
        unsigned ordinal = 1;
        for (const auto &slot : window) {
            const auto found = segments.find(slot.token);
            const auto count = found == segments.end() ? 1 : found->second.size();
            if (!count) throw std::invalid_argument("window parent has no segments");
            if (slot.token == token) {
                if (segmentIndex >= count) throw std::out_of_range("window segment");
                return ordinal + segmentIndex;
            }
            ordinal += count;
        }
        throw std::out_of_range("window parent");
    }
    // Produce a deterministic surgical edit plan. The caller must Browse and
    // fence every edit with the freshly observed UpdateID/URI vector. Keeping
    // a common anchor avoids removing an audible entry during future edits.
    static std::vector<Edit> edits(std::vector<Token> current, const std::vector<Token> &wanted,
                                   std::optional<Token> anchor = {}) {
        auto unique = [](auto tokens) {
            std::sort(tokens.begin(), tokens.end());
            return std::adjacent_find(tokens.begin(), tokens.end()) == tokens.end();
        };
        if (!unique(current) || !unique(wanted)) throw std::invalid_argument("duplicate window token");
        if (anchor && (std::find(current.begin(), current.end(), *anchor) == current.end() ||
                       std::find(wanted.begin(), wanted.end(), *anchor) == wanted.end()))
            throw std::invalid_argument("window anchor missing; explicit playback replacement required");
        // Unique occurrence tokens make the common subsequence an increasing
        // subsequence of wanted positions. Dynamic programming is bounded by
        // the configured row cap and avoids the old exponential six-row plan.
        std::map<Token, size_t> positions;
        for (size_t n = 0; n < wanted.size(); ++n) positions[wanted[n]] = n;
        std::vector<std::vector<Token>> paths(current.size());
        std::vector<Token> keep;
        for (size_t n = 0; n < current.size(); ++n) {
            if (!positions.count(current[n])) continue;
            paths[n] = {current[n]};
            for (size_t j = 0; j < n; ++j) {
                if (paths[j].empty() || positions.at(current[j]) >= positions.at(current[n])) continue;
                // Once past the anchor, a candidate must already include it.
                if (anchor && positions.at(current[n]) > positions.at(*anchor) &&
                    std::find(paths[j].begin(), paths[j].end(), *anchor) == paths[j].end()) continue;
                if (paths[j].size() + 1 > paths[n].size()) {
                    paths[n] = paths[j]; paths[n].push_back(current[n]);
                }
            }
            if (paths[n].size() > keep.size() && (!anchor ||
                std::find(paths[n].begin(), paths[n].end(), *anchor) != paths[n].end())) keep = paths[n];
        }
        std::vector<Edit> result;
        for (size_t n = current.size(); n; --n) {
            if (std::find(keep.begin(), keep.end(), current[n - 1]) != keep.end()) continue;
            result.push_back({Edit::Remove, unsigned(n), current[n - 1]});
            current.erase(current.begin() + n - 1);
        }
        for (unsigned n = 0; n < wanted.size(); ++n) {
            if (n < current.size() && current[n] == wanted[n]) continue;
            result.push_back({Edit::Insert, n + 1, wanted[n]});
            current.insert(current.begin() + n, wanted[n]);
        }
        return result;
    }
};
} // namespace queue_audio
