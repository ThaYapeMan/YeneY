// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_row_owner.h"
#include "test_temp.h"

#include <array>
#include <cassert>
#include <iostream>
#include <random>

using queue_audio::PlaylistEntry;
using queue_audio::QueueRecord;
using queue_audio::QueueRowOwner;

namespace {
constexpr unsigned rowCount = 3;
constexpr uint64_t token(unsigned index) { return index + 1; }

QueueRowOwner::StartSource source(unsigned index) {
    PlaylistEntry track;
    track.id = std::to_string(token(index));
    track.url = "file:///row-" + track.id + ".flac";
    return {std::move(track), int(index), token(index)};
}

void lateStart() {
    QueueRowOwner owner;
    const auto first = owner.nextRequest();
    assert(owner.commitSource(source(0), first, 1));
    owner.cancelRequest();
    const auto second = owner.nextRequest();
    assert(owner.commitSource(source(1), second, 2));
    const auto accepted = owner.sourceForStart();
    assert(!owner.commitSource(source(0), first, 1));
    assert(owner.sourceForStart() == accepted);
    assert(accepted->index == 1);
    std::cout << "PASS late LMS start leaves the current source and its exact request intact\n";
}

void exactAdmission() {
    QueueRowOwner owner;
    const auto first = owner.nextRequest();
    assert(owner.commitSource(source(0), first, 1));
    const auto old = owner.sourceForStart();
    owner.cancelRequest();
    const auto second = owner.nextRequest();
    assert(owner.commitSource(source(0), second, 2));
    const auto current = owner.sourceForStart();
    assert(current->cancellation > old->cancellation);

    QueueRecord late;
    late.source.occurrence = 2;
    late.sourceToken = token(0);
    late.sourceUrl = old->track.url;
    late.incomplete = "flushed";
    owner.recordSource(late.source.occurrence, old);
    assert(!owner.drainDeclarations({late}));

    QueueRecord settled = late;
    settled.source.occurrence = 3;
    owner.recordSource(settled.source.occurrence, current);
    assert(owner.drainDeclarations({late, settled}));
    std::cout << "PASS matching row and source still require the exact admission and cancellation generation\n";
}

void navigationGeneration() {
    QueueRowOwner owner;
    assert(owner.commitSource(source(0), owner.nextRequest(), 1));
    const auto old = owner.sourceForStart();
    owner.cancelRequest();
    owner.beginNavigation(2);
    assert(owner.commitSource(source(0), owner.nextRequest(), 2, true));
    const auto active = owner.sourceForStart();

    QueueRowOwner::Entry candidate{};
    candidate.uri = "/row-1.wav";
    candidate.stream = 2;
    owner.recordSource(2, old);
    assert(!owner.prepareNavigation(token(0), candidate));
    assert(!owner.navigationTarget());
    candidate.stream = 3;
    owner.recordSource(3, active);
    assert(owner.prepareNavigation(token(0), candidate));
    candidate.stream = 4;
    owner.recordSource(4, old);
    assert(!owner.prepareNavigation(token(0), candidate));
    assert(owner.navigationTarget()->stream == 3);
    assert(owner.navigationTarget()->uri == "/row-1.wav");
    std::cout << "PASS navigation accepts only the source generation that owns its target\n";
}

void cancelledFault() {
    QueueRowOwner owner;
    const auto serial = owner.nextRequest();
    assert(owner.commitSource(source(0), serial, 1));
    owner.cancelRequest();
    owner.cancelSourceThrough(1);
    const auto next = owner.nextRequest();
    assert(owner.commitSource(source(1), next, 2));
    QueueRecord record;
    record.source.occurrence = 1;
    record.fault = "truncated shared input";
    using Production = QueueRowOwner::Production;
    assert(owner.producerOutcome(record) == Production::Cancelled);
    assert(owner.sourceForStart()->index == 1);
    record.storageFault = true;
    assert(owner.producerOutcome(record) == Production::Faulted);
    record.storageFault = false;
    record.source.occurrence = 2;
    assert(owner.producerOutcome(record) == Production::Faulted);
    std::cout << "PASS cancelled source fault is inert and genuine storage/current faults surface\n";
}

void snapshotOrder() {
    using Decision = QueueRowOwner::SnapshotDecision;
    queue_audio::PlaylistSnapshot known;
    for (unsigned index = 0; index < rowCount; ++index) known.entries.push_back(source(index).track);
    auto partial = known;
    partial.entries.pop_back();
    assert(QueueRowOwner::snapshotDecision(known, partial, true) == Decision::RejectShort);
    queue_audio::PlaylistSnapshot empty;
    assert(QueueRowOwner::snapshotDecision(known, empty, true) == Decision::RejectShort);
    assert(QueueRowOwner::snapshotDecision(known, empty, false) == Decision::ConfirmClear);

    auto disjoint = empty;
    disjoint.entries.push_back(source(rowCount).track);
    assert(QueueRowOwner::snapshotDecision(known, disjoint, true) == Decision::ConfirmReplacement);
    assert(QueueRowOwner::confirmsReplacement(disjoint, disjoint));
    assert(!QueueRowOwner::confirmsReplacement(disjoint, partial));
    std::cout << "PASS short and empty snapshots cannot clear an active navigation\n";
}

struct TemporaryDirectory {
    std::string path = testTempRoot() + "/yeney-row-order-XXXXXX";
    TemporaryDirectory() { assert(mkdtemp(path.data())); }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};

void audioBinding() {
    TemporaryDirectory temporary;
    queue_audio::Config config;
    config.root = temporary.path;
    config.ramBytes = 4096 * 6;
    std::array<yeney::Frame, 4096> frames{};
    std::mt19937 generator(314159);
    for (auto &frame : frames) {
        frame = {int32_t(generator() & 0xffffff00u), int32_t(generator() & 0xffffff00u)};
    }
    auto produce = [&](queue_audio::Store &store, const std::string &name, uint64_t generation) {
        auto item = store.create({name, 44100, frames.size(), generation});
        item->append(frames.data(), frames.size(), false);
        item->finish();
        return item;
    };
    {
        queue_audio::Store estimate("Estimate", config);
        produce(estimate, "sample", 1);
        const auto bytes = estimate.diskBytes();
        assert(bytes != 0);
        config.diskBytes = bytes * 2 + bytes / 2;
    }
    queue_audio::Store store("Room", config);
    QueueRowOwner owner;
    const auto row = owner.prepareLazyRow([] {});
    owner.registerLazyRow(token(0), row);
    auto first = produce(store, "first", 1);
    auto other = produce(store, "other", 2);
    assert(row->publish(first, 1));
    QueueRowOwner::Entry retained{};
    retained.item = first;
    retained.originMs = 1000;
    QueueRecord seek;
    seek.item = first;
    seek.landedFrame = 1100;
    assert(!QueueRowOwner::seekWithin(retained, seek, 1025));
    seek.pairedFrom = 1;
    assert(!QueueRowOwner::seekWithin(retained, seek, 975));
    assert(QueueRowOwner::seekWithin(retained, seek, 1025) == 25);
    const auto metadata = row->ready();
    auto response = row->readyAudio();
    assert(response == first && metadata == first);
    row->alias(first, 1, 2);
    auto wrongLength = store.create({"wrong-length", 44100, frames.size() + 1, 2});
    assert(!row->replace(first, 1, wrongLength, 3));
    bool rejected = false;
    try {
        row->replace(first, 2, wrongLength, 3);
    } catch (const queue_audio::LazyItem::DeclarationMismatch &) {
        rejected = true;
    }
    assert(rejected && row->ready() == first);

    auto read = [](const auto &item) {
        const auto length = item->contentLength().value();
        std::vector<char> bytes(length);
        auto reader = item->reader();
        size_t offset = 0;
        while (offset < length) {
            const size_t amount = reader.read(bytes.data() + offset, length - offset,
                                              std::chrono::milliseconds(0));
            assert(amount);
            offset += amount;
        }
        return bytes;
    };

    auto replacement = produce(store, "replacement", 3);
    assert(row->replace(first, 2, replacement, 3));
    assert(!row->replace(first, 1, first, 2));
    assert(other->evicted() && !first->evicted());
    assert(row->readyAudio() == replacement && response == first);
    assert(read(response) == read(replacement));
    assert(store.diskBytes() <= config.diskBytes);

    response.reset();
    produce(store, "pressure-after-close", 4);
    assert(metadata->evicted());
    assert(row->readyAudio() == replacement);
    std::cout << "PASS HTTP body retains the admitted generation across replacement and quota pressure\n";
}

enum class Operation {
    App, Rewind, Issue, Start, LateStart, Cancel, Publish, Observe, End,
    Progress, Complete, Fault, HttpOpen, Body, HttpClose, EvictionPressure,
    LmsNavigation, SeekInside, SeekPast, Restart, NaturalEnd
};
constexpr unsigned operationCount = 9;
constexpr unsigned alphabetSize = operationCount * rowCount;

constexpr unsigned lifecycleAlphabetSize = 21 * rowCount;

struct AudioFixture {
    TemporaryDirectory temporary;
    queue_audio::Store store;
    std::array<std::shared_ptr<queue_audio::Item>, rowCount> assets;
    std::array<std::vector<char>, rowCount> bytes;

    AudioFixture() : store("RowReplay", {temporary.path}) {
        for (unsigned index = 0; index < rowCount; ++index) {
            auto item = store.create({std::to_string(index), 44100, 8, token(index)});
            std::array<yeney::Frame, 8> frames{};
            for (auto &frame : frames) frame.left = int32_t(token(index) * 65536);
            item->append(frames.data(), frames.size(), false);
            item->finish();
            assets[index] = item;
            bytes[index] = read(*item);
        }
    }

    static std::vector<char> read(const queue_audio::Item &item) {
        std::vector<char> result(item.contentLength().value());
        auto reader = item.reader();
        size_t offset = 0;
        while (offset < result.size()) {
            const auto copied = reader.read(result.data() + offset, result.size() - offset,
                                            std::chrono::milliseconds(0));
            assert(copied);
            offset += copied;
        }
        return result;
    }
};

struct Lifecycle {
    const AudioFixture &fixture;
    std::array<std::optional<QueueRecord>, rowCount> productions;
    std::array<std::shared_ptr<queue_audio::Item>, rowCount> responses;
    std::optional<unsigned> navigation;
};

class Replay {
    QueueRowOwner owner;
    uint64_t tick = 0;
    uint64_t nextOccurrence = 1;
    uint64_t previousRequest = 0;
    uint64_t latestAccepted = 0;
    unsigned latestIndex = 0;

    std::unique_ptr<Lifecycle> lifecycle;

    void acceptStart(unsigned index, bool navigation = false) {
        previousRequest = latestAccepted;
        latestAccepted = owner.nextRequest();
        latestIndex = index;
        assert(owner.commitSource(source(index), latestAccepted, nextOccurrence, navigation));
        owner.recordSource(nextOccurrence, owner.sourceForStart());
        if (lifecycle) {
            QueueRecord record;
            record.source.occurrence = nextOccurrence;
            record.sourceToken = token(index);
            record.sourceUrl = source(index).track.url;
            lifecycle->productions[index] = std::move(record);
        }
        ++nextOccurrence;
    }

    void cancel() {
        owner.cancelSourceThrough(nextOccurrence - 1);
        owner.cancelRequest();
        owner.clearAdmissions();
        previousRequest = latestAccepted;
        if (lifecycle) {
            for (auto &record : lifecycle->productions) {
                if (record && !record->productionComplete) record->incomplete = "flushed";
            }
            lifecycle->navigation.reset();
            owner.clearNavigationTarget();
        }
    }

    void advanceProduction(unsigned index, bool complete) {
        auto &producer = lifecycle->productions[index];
        if (!producer) return;
        auto &record = *producer;
        if (record.productionComplete && !complete) return;
        record.item = lifecycle->fixture.assets[index];
        record.actual = complete ? 8 : 4;
        record.productionComplete = complete;
        using Production = QueueRowOwner::Production;
        const auto outcome = owner.producerOutcome(record);
        if (outcome != Production::Writing && outcome != Production::Complete) return;

        const auto &previous = owner.layout().at(index);
        if (lifecycle->navigation == index) {
            auto target = previous;
            target.item = record.item;
            target.stream = record.source.occurrence;
            target.frames = 8;
            if (!owner.prepareNavigation(token(index), target)) return;
            assert(owner.sourceIndex(target.stream) == index);
            assert(owner.navigationTarget()->uri == previous.uri);
            assert(owner.navigationTarget()->stream == target.stream);
            if (!previous.item) assert(owner.reproduceNavigation(target));
            else {
                target = previous;
                owner.rebindClock(target, record.source.occurrence);
            }
            auto layout = owner.layout();
            layout[index] = std::move(target);
            owner.commitLayout(std::move(layout));
            lifecycle->navigation.reset();
            owner.clearNavigationTarget();
        } else if (!previous.item) {
            assert(owner.reproduce(index, token(index), record, 8));
        } else if (previous.stream != record.source.occurrence) {
            assert(previous.item == record.item);
            owner.rebindClock(index, record.source.occurrence);
        }
    }

    void enqueue(unsigned index, std::optional<uint64_t> rewind) {
        if (!owner.selectionAt(index)) owner.enqueueSelection(index, token(index), rewind);
    }

public:
    explicit Replay(const AudioFixture *fixture = nullptr) {
        if (!fixture) return;
        lifecycle = std::make_unique<Lifecycle>(Lifecycle{*fixture, {}, {}, {}});
        std::vector<QueueRowOwner::Entry> layout;
        for (unsigned index = 0; index < rowCount; ++index) {
            const auto number = token(index);
            const auto placeholder = UINT64_MAX - number;
            owner.registerPlaceholder(placeholder, index, number);
            owner.registerLazyRow(number, owner.prepareLazyRow([] {}));
            layout.push_back({placeholder, "/row-" + std::to_string(number) + ".wav",
                              {}, source(index).track.url, 0, {}, 0, 0, {}, 0});
        }
        owner.commitLayout(std::move(layout));
    }

    void apply(unsigned symbol) {
        const auto operation = Operation(symbol / rowCount);
        const auto index = symbol % rowCount;
        ++tick;
        const auto before = owner.sourceForStart();
        switch (operation) {
        case Operation::App:
            enqueue(index, {});
            break;
        case Operation::Rewind:
            enqueue(index, 1000);
            break;
        case Operation::Issue:
            if (owner.hasProducingSelection()) break;
            while (const auto next = owner.dequeueSelection()) {
                const auto selection = owner.selectionAt(*next);
                if (!selection || selection->phase != QueueRowOwner::Selection::Queued) continue;
                const auto now = std::chrono::steady_clock::time_point(std::chrono::milliseconds(tick));
                const auto issued = owner.issueSelection(*next, nextOccurrence, now);
                assert(issued.token == token(*next));
                assert(issued.first == nextOccurrence);
                if (issued.rewindMs) assert(issued.issued == now);
                break;
            }
            break;
        case Operation::Start:
            acceptStart(index);
            break;
        case Operation::LateStart:
            if (!previousRequest) break;
            assert(!owner.commitSource(source(index), previousRequest, nextOccurrence));
            assert(owner.sourceForStart() == before);
            break;
        case Operation::Cancel:
            cancel();
            break;
        case Operation::Publish:
            owner.acknowledgePublication(index);
            break;
        case Operation::Observe:
            owner.acknowledgeObservation(index, token(index), nextOccurrence - 1);
            break;
        case Operation::End:
            owner.retireSelection(index);
            break;
        case Operation::Progress:
            advanceProduction(index, false);
            break;
        case Operation::Complete:
            advanceProduction(index, true);
            break;
        case Operation::Fault:
            if (auto &record = lifecycle->productions[index]) {
                const auto previous = owner.layout()[index].stream;
                record->fault = "source failed";
                using Production = QueueRowOwner::Production;
                const auto result = owner.producerOutcome(*record);
                assert(result == Production::Cancelled || result == Production::Faulted);
                assert(owner.layout()[index].stream == previous);
            }
            break;
        case Operation::HttpOpen:
            lifecycle->responses[index] = owner.lazyRow(token(index))->readyAudio();
            break;
        case Operation::Body:
            if (const auto &response = lifecycle->responses[index])
                assert(AudioFixture::read(*response) == lifecycle->fixture.bytes[index]);
            break;
        case Operation::HttpClose:
            lifecycle->responses[index].reset();
            break;
        case Operation::EvictionPressure:
            if (const auto &entry = owner.layout()[index]; entry.item && entry.item->evicted())
                assert(owner.invalidateAudio(token(index), index));
            break;
        case Operation::LmsNavigation:
        case Operation::Restart:
            cancel();
            owner.beginNavigation(nextOccurrence);
            lifecycle->navigation = index;
            acceptStart(index, true);
            break;
        case Operation::SeekInside:
        case Operation::SeekPast:
            if (const auto &row = owner.layout()[index]; row.item) {
                QueueRecord probe;
                probe.item = row.item;
                probe.landedFrame = 0;
                probe.pairedFrom = operation == Operation::SeekInside ? row.stream : 0;
                assert(QueueRowOwner::seekWithin(row, probe, row.originMs).has_value() ==
                       (operation == Operation::SeekInside));
            }
            break;
        case Operation::NaturalEnd:
            owner.retireSelection(index);
            if (latestAccepted && latestIndex == index) acceptStart((index + 1) % rowCount);
            break;
        }

        if (lifecycle) {
            for (unsigned row = 0; row < rowCount; ++row) {
                const auto &entry = owner.layout()[row];
                assert(entry.uri == "/row-" + std::to_string(token(row)) + ".wav");
                assert(entry.item == owner.lazyRow(token(row))->ready());
                if (entry.item) {
                    assert(entry.item == lifecycle->fixture.assets[row]);
                    assert(entry.item->contentLength() == lifecycle->fixture.bytes[row].size());
                }
                if (const auto &response = lifecycle->responses[row]) {
                    assert(response == lifecycle->fixture.assets[row]);
                    assert(!response->evicted());
                }
            }
        }
        unsigned producing = 0;
        for (const auto &[selectedIndex, selection] : owner.selectionSnapshot()) {
            assert(selectedIndex < rowCount);
            assert(selection.token == token(selectedIndex));
            producing += selection.phase == QueueRowOwner::Selection::Producing;
            assert(selection.phase == QueueRowOwner::Selection::Queued || selection.first != 0);
        }
        assert(producing <= 1);
        if (latestAccepted) {
            const auto accepted = owner.sourceForStart();
            assert(accepted->request == latestAccepted);
            assert(accepted->index == int(latestIndex));
            assert(accepted->token == token(latestIndex));
        }
    }
};

template <size_t Size>
void orders(std::array<unsigned, Size> &sequence, size_t count, uint64_t &checked) {
    Replay replay;
    for (size_t offset = 0; offset < count; ++offset) replay.apply(sequence[offset]);
    ++checked;
    if (count == Size) return;
    for (unsigned symbol = 0; symbol < alphabetSize; ++symbol) {
        sequence[count] = symbol;
        orders(sequence, count + 1, checked);
    }
}

void exhaustive() {
    std::array<unsigned, 5> sequence{};
    uint64_t checked = 0;
    orders(sequence, 0, checked);
    std::cout << "PASS three-row admission and selection orders N=" << sequence.size()
              << " alphabet=" << alphabetSize << " orders=" << checked << '\n';
}

void seeded() {
    for (uint32_t seed : {314159u, 8675309u, 271828u}) {
        std::mt19937 generator(seed);
        Replay replay;
        for (unsigned event = 0; event < 2000; ++event) {
            replay.apply(generator() % alphabetSize);
        }
        std::cout << "PASS three-row admission and selection seed=" << seed << " events=2000\n";
    }
}
void lifecycleOrders(const AudioFixture &fixture, std::array<unsigned, 4> &sequence,
                     size_t count, uint64_t &checked, unsigned shard) {
    Replay replay(&fixture);
    for (size_t offset = 0; offset < count; ++offset) replay.apply(sequence[offset]);
    if (count || !shard) ++checked;
    if (count == sequence.size()) return;
    for (unsigned symbol = 0; symbol < lifecycleAlphabetSize; ++symbol) {
        if (!count && symbol % rowCount != shard) continue;
        sequence[count] = symbol;
        lifecycleOrders(fixture, sequence, count + 1, checked, shard);
    }
}

void lifecycleExhaustive(unsigned shard) {
    AudioFixture fixture;
    std::array<unsigned, 4> sequence{};
    uint64_t checked = 0;
    lifecycleOrders(fixture, sequence, 0, checked, shard);
    std::cout << "PASS lifecycle orders N=" << sequence.size() << " alphabet=" << lifecycleAlphabetSize
              << " shard=" << shard << " orders=" << checked << '\n';
}

void lifecycleSeeded() {
    AudioFixture fixture;
    for (uint32_t seed : {314159u, 8675309u, 271828u}) {
        std::mt19937 generator(seed);
        Replay replay(&fixture);
        for (unsigned event = 0; event < 2000; ++event)
            replay.apply(generator() % lifecycleAlphabetSize);
        std::cout << "PASS three-row lifecycle seed=" << seed << " events=2000\n";
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const std::string name = argv[1];
    if (name == "late-start") lateStart();
    else if (name == "exact-admission") exactAdmission();
    else if (name == "navigation-generation") navigationGeneration();
    else if (name == "cancelled-fault") cancelledFault();
    else if (name == "snapshot-order") snapshotOrder();
    else if (name == "audio-binding") audioBinding();
    else if (name == "exhaustive") exhaustive();
    else if (name == "seeded") seeded();
    else if (name == "lifecycle-exhaustive-0") lifecycleExhaustive(0);
    else if (name == "lifecycle-exhaustive-1") lifecycleExhaustive(1);
    else if (name == "lifecycle-exhaustive-2") lifecycleExhaustive(2);
    else if (name == "lifecycle-seeded") lifecycleSeeded();
    else return 2;
}
