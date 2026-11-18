// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "core_queue_sink.h"
#include "queue_lazy_item.h"
#include "queue_playlist.h"
#include <algorithm>
#include <atomic>
#include <map>

namespace queue_audio {

class QueueRowOwner {
public:
    struct Entry {
        uint64_t stream;
        std::string uri;
        std::shared_ptr<Item> item;
        std::string sourceUrl;
        uint64_t frames;
        std::string title;
        uint64_t owner = 0, segmentOffset = 0;
        std::string didl;
        unsigned originMs = 0;
    };

    struct StartSource {
        PlaylistEntry track;
        int index = -1;
        uint64_t token = 0;
        uint32_t gain = 0;
    };

    struct Admission {
        uint64_t token;
        std::string sourceUrl;
        uint64_t request;
        uint64_t cancelledThrough;
        uint64_t firstOccurrence;
    };

    // Request changes are issued under the room command mutex.
    uint64_t nextRequest() { return ++request; }
    uint64_t currentRequest() const { return request.load(); }

    void cancelRequest() {
        const auto serial = nextRequest();
        if (commandStart) {
            commandStart->store(serial + 1);
            commandStart.reset();
        }
    }

    std::shared_ptr<std::atomic<uint64_t>> expectCommandStart() {
        commandStart = std::make_shared<std::atomic<uint64_t>>(0);
        return commandStart;
    }

    void retireCommandStart() { commandStart.reset(); }

    void cancelSourceThrough(uint64_t occurrence) { cancelledThrough = occurrence; }

    bool sourceFaultSuperseded(uint64_t occurrence) const {
        return occurrence <= cancelledThrough;
    }

    // The room worker advances admissions in token order.
    void admit(uint64_t token, const std::string &sourceUrl, uint64_t serial,
               uint64_t firstOccurrence) {
        admissions.insert_or_assign(token, Admission{
            token, sourceUrl, serial, cancelledThrough.load(), firstOccurrence});
    }

    void commitSource(StartSource source, uint64_t serial, uint64_t firstOccurrence) {
        if (firstOccurrence && source.token)
            admit(source.token, source.track.url, serial, firstOccurrence);
        std::atomic_store(&activeSource, std::make_shared<const StartSource>(std::move(source)));
    }

    std::shared_ptr<const StartSource> sourceForStart() const {
        return std::atomic_load(&activeSource);
    }

    bool drainDeclarations(const std::vector<QueueRecord> &records) {
        for (auto entry = admissions.begin(); entry != admissions.end();) {
            const auto &admission = entry->second;
            const auto declaring = std::find_if(records.begin(), records.end(),
                [&admission](const auto &record) {
                    return record.source.occurrence >= admission.firstOccurrence &&
                           record.sourceToken == admission.token;
                });
            if (declaring == records.end() ||
                (declaring->incomplete.empty() && declaring->fault.empty() &&
                 (!declaring->item || (declaring->pairedFrom &&
                  declaring->actual - declaring->attachOffset < declaring->verificationFrames))))
                return false;
            entry = admissions.erase(entry);
        }
        return true;
    }

    void clearAdmissions() { admissions.clear(); }

    void recordSource(uint64_t occurrence, const StartSource &source) {
        std::lock_guard<std::mutex> lock(sourceMutex);
        auto &record = sources[occurrence];
        record.track = source.track;
        if (source.index >= 0) record.index = unsigned(source.index);
        if (source.token) record.token = source.token;
    }

    void locate(uint64_t occurrence, unsigned index) {
        std::lock_guard<std::mutex> lock(sourceMutex);
        sources[occurrence].index = index;
    }

    void registerPlaceholder(uint64_t occurrence, unsigned index, uint64_t token) {
        std::lock_guard<std::mutex> lock(sourceMutex);
        auto &record = sources[occurrence];
        record.index = index;
        record.token = token;
    }

    std::optional<PlaylistEntry> sourceTrack(uint64_t occurrence) const {
        std::lock_guard<std::mutex> lock(sourceMutex);
        const auto record = sources.find(occurrence);
        return record == sources.end() ? std::nullopt : record->second.track;
    }

    std::optional<unsigned> sourceIndex(uint64_t occurrence) const {
        std::lock_guard<std::mutex> lock(sourceMutex);
        const auto record = sources.find(occurrence);
        return record == sources.end() ? std::nullopt : record->second.index;
    }

    std::optional<uint64_t> sourceToken(uint64_t occurrence) const {
        std::lock_guard<std::mutex> lock(sourceMutex);
        const auto record = sources.find(occurrence);
        return record == sources.end() ? std::nullopt : record->second.token;
    }

    std::vector<uint64_t> earlierSources(uint64_t token, uint64_t before) const {
        std::lock_guard<std::mutex> lock(sourceMutex);
        std::vector<uint64_t> matching;
        for (auto entry = sources.begin(); entry != sources.end() && entry->first < before; ++entry)
            if (entry->second.token == token) matching.push_back(entry->first);
        return matching;
    }

    // The worker owns the token-to-route lookup. A route retains its own
    // binding until outstanding HTTP requests have returned.
    std::shared_ptr<LazyItem> lazyRow(uint64_t token) const {
        const auto row = lazyRows.find(token);
        return row == lazyRows.end() ? nullptr : row->second;
    }

    LazyItem &declaredLazyRow(uint64_t token) const { return *lazyRows.at(token); }

    void registerLazyRow(uint64_t token, const std::shared_ptr<LazyItem> &row) {
        lazyRows.insert_or_assign(token, row);
    }

    bool invalidateAudio(uint64_t token, Entry &entry) {
        const auto row = lazyRow(token);
        if (!row) return false;
        row->reset(entry.item);
        entry.stream = UINT64_MAX - token;
        entry.item.reset();
        entry.owner = 0;
        entry.frames = 0;
        entry.originMs = 0;
        return true;
    }

    void retireLazyRows() {
        for (const auto &row : lazyRows)
            row.second->cancel();
        lazyRows.clear();
    }

private:
    std::shared_ptr<const StartSource> activeSource = std::make_shared<const StartSource>();
    std::atomic<uint64_t> request{0};
    std::shared_ptr<std::atomic<uint64_t>> commandStart;
    std::atomic<uint64_t> cancelledThrough{0};
    std::map<uint64_t, Admission> admissions;
    struct Source {
        std::optional<PlaylistEntry> track;
        std::optional<unsigned> index;
        std::optional<uint64_t> token;
    };
    mutable std::mutex sourceMutex;
    std::map<uint64_t, Source> sources;
    std::map<uint64_t, std::shared_ptr<LazyItem>> lazyRows;
};

} // namespace queue_audio
