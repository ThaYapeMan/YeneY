// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "core_queue_sink.h"
#include "queue_lazy_item.h"
#include "queue_playlist.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>

namespace queue_audio {

class QueueRowOwner {
public:
    struct Selection {
        enum Phase { Queued, Producing, Published } phase;
        uint64_t token, first;
        std::optional<uint64_t> observed{};
        std::optional<uint64_t> rewindMs{};
        std::chrono::steady_clock::time_point issued{};
        std::shared_ptr<std::atomic<uint64_t>> commandRequest{};
    };

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
        uint64_t request = 0, cancellation = 0;
    };

    struct Admission {
        uint64_t token;
        std::string sourceUrl;
        uint64_t request;
        uint64_t cancellation;
        uint64_t firstOccurrence;
    };

    // Request changes are issued under the room command mutex.
    uint64_t nextRequest() { return ++request; }
    uint64_t currentRequest() const { return request.load(); }

    void cancelRequest() {
        const auto serial = nextRequest();
        cancellation = serial;
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

    enum class Production { Pending, Writing, Complete, Cancelled, Evicted, Faulted };

    Production producerOutcome(const QueueRecord &record) const {
        // A parked or cancelled response may still have a valid WAV handle.
        if (record.item && !record.incomplete.empty()) return Production::Cancelled;
        if (record.item && record.item->evicted()) return Production::Evicted;
        const auto parent = record.parent ? record.parent : record.source.occurrence;
        if (!record.fault.empty()) {
            if (!record.storageFault && parent <= cancelledThrough)
                return Production::Cancelled;
            return Production::Faulted;
        }
        if (!record.item || !record.incomplete.empty() ||
            (record.pairedFrom && record.actual - record.attachOffset < record.verificationFrames))
            return Production::Pending;
        return record.productionComplete ? Production::Complete : Production::Writing;
    }

    bool commitSource(StartSource source, uint64_t serial, uint64_t firstOccurrence, bool navigation = false) {
        if (serial != currentRequest()) return false;
        source.request = serial;
        source.cancellation = cancellation;
        if (firstOccurrence && source.token)
            admissions.insert_or_assign(source.token, Admission{
                source.token, source.track.url, serial, cancellation, firstOccurrence});
        auto committed = std::make_shared<const StartSource>(std::move(source));
        if (navigation) {
            navigationRowToken = committed->token;
            navigationSource = committed;
        }
        std::atomic_store(&activeSource, std::move(committed));
        return true;
    }

    std::shared_ptr<const StartSource> sourceForStart() const {
        return std::atomic_load(&activeSource);
    }

    bool drainDeclarations(const std::vector<QueueRecord> &records) {
        for (auto entry = admissions.begin(); entry != admissions.end();) {
            const auto &admission = entry->second;
            const auto declaring = std::find_if(records.begin(), records.end(),
                [this, &admission](const auto &record) {
                    return record.source.occurrence >= admission.firstOccurrence &&
                           record.sourceToken == admission.token &&
                           record.sourceUrl == admission.sourceUrl &&
                           matchesAdmission(record.source.occurrence, admission);
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

    // Selection orders and acknowledgements are applied by the room worker.
    std::optional<Selection> selectionAt(unsigned index) const {
        const auto found = selections.find(index);
        return found == selections.end() ? std::nullopt : std::optional<Selection>(found->second);
    }

    std::map<unsigned, Selection> selectionSnapshot() const { return selections; }
    bool hasSelections() const { return !selections.empty(); }

    bool waitingForSelectionPublication() const {
        return std::any_of(selections.begin(), selections.end(),
            [](const auto &entry) { return entry.second.phase != Selection::Published; });
    }

    bool hasProducingSelection() const {
        return std::any_of(selections.begin(), selections.end(),
            [](const auto &entry) { return entry.second.phase == Selection::Producing; });
    }

    void bindSelection(unsigned index, uint64_t token, uint64_t first) {
        selections.insert_or_assign(index, Selection{Selection::Producing, token, first});
    }

    void enqueueSelection(unsigned index, uint64_t token, std::optional<uint64_t> rewind = {}) {
        selections.insert_or_assign(index, Selection{Selection::Queued, token, 0, {}, rewind});
        selectionOrder.push_back(index);
    }

    bool queuedSelectionForOtherRow(uint64_t token) const {
        return std::any_of(selectionOrder.begin(), selectionOrder.end(), [&](unsigned index) {
            const auto found = selections.find(index);
            return found != selections.end() && found->second.token != token;
        });
    }

    std::optional<unsigned> dequeueSelection() {
        if (selectionOrder.empty()) return std::nullopt;
        const auto index = selectionOrder.front();
        selectionOrder.pop_front();
        return index;
    }

    void deferSelection(unsigned index) { selectionOrder.push_front(index); }
    void retireSelection(unsigned index) { selections.erase(index); }

    Selection issueSelection(unsigned index, uint64_t first, std::chrono::steady_clock::time_point now) {
        auto &selection = selections.at(index);
        selection.phase = Selection::Producing;
        selection.first = first;
        selection.commandRequest = expectCommandStart();
        if (selection.rewindMs) selection.issued = now;
        return selection;
    }

    bool acknowledgePublication(unsigned index) {
        const auto selection = selections.find(index);
        if (selection == selections.end() || selection->second.phase != Selection::Producing) return false;
        const bool observed = selection->second.observed.has_value();
        const auto first = selection->second.first;
        selection->second.phase = Selection::Published;
        if (observed) retirePublishedThrough(first);
        return true;
    }

    void acknowledgeObservation(unsigned index, uint64_t token, uint64_t occurrence) {
        const auto selection = selections.find(index);
        if (selection == selections.end() || selection->second.token != token) return;
        selection->second.observed = occurrence;
        if (selection->second.phase == Selection::Published)
            retirePublishedThrough(selection->second.first);
    }

    void clearSelections() {
        selections.clear();
        selectionOrder.clear();
    }

    void clearAdmissions() { admissions.clear(); }

    void recordSource(uint64_t occurrence, std::shared_ptr<const StartSource> source) {
        std::lock_guard<std::mutex> lock(sourceMutex);
        auto &record = sources[occurrence];
        if (source->index >= 0) record.index = unsigned(source->index);
        if (source->token) record.token = source->token;
        record.start = std::move(source);
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
        if (record == sources.end() || !record->second.start) return std::nullopt;
        return record->second.start->track;
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

    std::shared_ptr<LazyItem> prepareLazyRow(std::function<void()> activate) const {
        return std::make_shared<LazyItem>(nullptr, std::move(activate), true, lazyMutex);
    }

    LazyItem &declaredLazyRow(uint64_t token) const { return *lazyRows.at(token); }

    void registerLazyRow(uint64_t token, const std::shared_ptr<LazyItem> &row) {
        lazyRows.insert_or_assign(token, row);
    }

    uint64_t navigationFirst() const { return firstNavigationOccurrence; }
    uint64_t navigationToken() const { return navigationRowToken; }
    const std::optional<Entry> &navigationTarget() const { return preparedNavigation; }

    void beginNavigation(uint64_t first) {
        firstNavigationOccurrence = first;
        navigationSource.reset();
        preparedNavigation.reset();
    }

    bool prepareNavigation(uint64_t token, Entry item) {
        if (token != navigationRowToken || item.stream < firstNavigationOccurrence) return false;
        {
            std::lock_guard<std::mutex> lock(sourceMutex);
            const auto found = sources.find(item.stream);
            if (found == sources.end() || !navigationSource || found->second.start != navigationSource)
                return false;
        }
        firstNavigationOccurrence = item.stream;
        preparedNavigation = std::move(item);
        return true;
    }

    void clearNavigationTarget() { preparedNavigation.reset(); }

    bool reproduceNavigation(Entry &row) {
        const auto &item = preparedNavigation.value();
        if (!declaredLazyRow(navigationRowToken).publish(item.item, item.stream)) return false;
        row.stream = item.stream;
        row.item = item.item;
        row.frames = item.frames;
        row.originMs = item.originMs;
        return true;
    }

    bool replaceNavigationWav(const Entry &expected, const Entry &replacement) {
        return declaredLazyRow(navigationRowToken).replace(expected.item, expected.stream,
                                                          replacement.item, replacement.stream);
    }

    void rebindClock(Entry &entry, uint64_t occurrence) {
        const auto token = sourceToken(entry.owner ? entry.owner : entry.stream);
        if (token) {
            const auto row = lazyRow(*token);
            if (row) row->alias(entry.item, entry.stream, occurrence);
        }
        if (!entry.owner) entry.owner = entry.stream;
        entry.stream = occurrence;
    }

    const std::vector<Entry> &layout() const { return committedRows; }

    void commitLayout(std::vector<Entry> rows) { committedRows.swap(rows); }

    void insertRow(size_t row, Entry entry) {
        committedRows.insert(committedRows.begin() + row, std::move(entry));
    }

    void rebindClock(size_t row, uint64_t occurrence) {
        rebindClock(committedRows.at(row), occurrence);
    }

    bool reproduce(size_t row, uint64_t token, const QueueRecord &record, uint64_t frames) {
        auto &entry = committedRows.at(row);
        if (!declaredLazyRow(token).publish(record.item, record.source.occurrence)) return false;
        entry.stream = record.source.occurrence;
        entry.item = record.item;
        entry.frames = frames;
        entry.originMs = record.originMs;
        return true;
    }

    bool invalidateAudio(uint64_t token, size_t row) {
        return invalidateAudio(token, committedRows.at(row));
    }

    bool invalidateAudio(uint64_t token, Entry &entry) {
        const auto row = lazyRow(token);
        if (!row) return false;
        row->reset(entry.item, entry.stream);
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
    void retirePublishedThrough(uint64_t first) {
        for (auto selection = selections.begin(); selection != selections.end();) {
            if (selection->second.phase == Selection::Published && selection->second.first <= first)
                selection = selections.erase(selection);
            else ++selection;
        }
    }

    uint64_t firstNavigationOccurrence = 0;
    uint64_t navigationRowToken = 0;
    std::shared_ptr<const StartSource> navigationSource;
    std::optional<Entry> preparedNavigation;
    std::vector<Entry> committedRows;
    std::map<unsigned, Selection> selections;
    std::deque<unsigned> selectionOrder;
    std::shared_ptr<const StartSource> activeSource = std::make_shared<const StartSource>();
    std::atomic<uint64_t> request{0};
    uint64_t cancellation = 0;
    std::shared_ptr<std::atomic<uint64_t>> commandStart;
    std::atomic<uint64_t> cancelledThrough{0};
    std::map<uint64_t, Admission> admissions;
    struct Source {
        std::shared_ptr<const StartSource> start;
        std::optional<unsigned> index;
        std::optional<uint64_t> token;
    };
    bool matchesAdmission(uint64_t occurrence, const Admission &admission) const {
        std::lock_guard<std::mutex> lock(sourceMutex);
        const auto source = sources.find(occurrence);
        if (source == sources.end() || !source->second.start) return false;
        const auto &start = *source->second.start;
        return start.request == admission.request && start.cancellation == admission.cancellation;
    }

    mutable std::mutex sourceMutex;
    std::map<uint64_t, Source> sources;
    std::map<uint64_t, std::shared_ptr<LazyItem>> lazyRows;
    const std::shared_ptr<std::mutex> lazyMutex = std::make_shared<std::mutex>();
};

} // namespace queue_audio
