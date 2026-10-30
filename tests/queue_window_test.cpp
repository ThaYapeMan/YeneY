// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_window.h"
#include "queue_window_edits.h"
#include <cassert>
#include <cstdio>
#include <numeric>
using queue_audio::QueueWindow;
using Token = QueueWindow::Token;
static void checkPlan(std::vector<Token> current, const std::vector<Token> &wanted,
                      std::optional<Token> anchor = {}) {
    for (const auto &edit : QueueWindow::edits(current, wanted, anchor)) {
        assert(edit.ordinal && edit.ordinal <= current.size() + 1);
        if (edit.kind == QueueWindow::Edit::Remove) {
            assert(current.at(edit.ordinal - 1) == edit.token);
            assert(!anchor || edit.token != *anchor);
            current.erase(current.begin() + edit.ordinal - 1);
        } else current.insert(current.begin() + edit.ordinal - 1, edit.token);
    }
    assert(current == wanted);
}
int main() {
    queue_audio::PlaylistSnapshot snapshot;
    for (unsigned n = 0; n < 10; ++n) {
        queue_audio::PlaylistEntry track;
        track.id = std::to_string(n); track.url = "file:///" + track.id;
        track.duration = 120; track.title = "Track " + track.id;
        snapshot.entries.push_back(track);
    }
    const auto status = queue_audio::PlaylistReader().parse(
        "playlist_tracks:0 playlist_cur_index:-1 playlist_timestamp:17.5 playlist%20shuffle:2 playlist%20repeat:1");
    assert(status.revision == "17.5" && status.shuffle == 2 && status.repeat == 1);
    bool invalidPolicy = false;
    try { queue_audio::PlaylistReader().parse("playlist_tracks:0 playlist%20repeat:3"); }
    catch (const std::runtime_error &) { invalidPolicy = true; }
    assert(invalidPolicy);
    snapshot.revision = "18"; snapshot.shuffle = 1; snapshot.repeat = 2;
    QueueWindow model;
    model.reconcile(snapshot);
    assert(model.policy().revision == "18" && model.policy().shuffle == 1 && model.policy().repeat == 2);
    auto window = model.around(4);
    assert(window.size() == 6 && window.front().index == 2 && window.back().index == 7);
    assert(QueueWindow::atOrdinal(window, {}, 3).slot.index == 4);
    QueueWindow::Segments segments{{window[1].token, {{41,0},{42,441000}}}};
    auto continuation = QueueWindow::atOrdinal(window, segments, 3);
    assert(continuation.slot.index == 3 && continuation.segmentIndex == 1);
    assert(continuation.segment.occurrence == 42 && continuation.segment.offsetFrames == 441000);
    assert(QueueWindow::atOrdinal(window, segments, 4).slot.index == 4);
    assert(QueueWindow::ordinalOf(window, segments, window[2].token) == 4);
    assert(QueueWindow::ordinalOf(window, segments, window[1].token, 1) == 3);
    assert(window.size() == 6); // continuation does not consume a history slot
    auto current = window[2].token;
    snapshot.entries.insert(snapshot.entries.begin(), snapshot.entries.back());
    snapshot.entries.erase(snapshot.entries.begin() + 3);
    std::swap(snapshot.entries[6], snapshot.entries[7]);
    model.reconcile(snapshot);
    assert(model.find(current)->track.id == "4");
    assert(model.find(current)->index == 4);
    snapshot.entries[5].duration = 0;
    model.reconcile(snapshot);
    assert(model.around(4).back().index == 4);
    snapshot.entries[5].duration = 100;
    snapshot.entries[5].liveEdge = 0;
    model.reconcile(snapshot);
    assert(model.around(4).back().index == 4);
    snapshot.entries.clear(); model.reconcile(snapshot);
    assert(model.around(0).empty() && !model.find(current));
    snapshot.entries = {window[0].track,window[0].track,window[1].track};
    model.reconcile(snapshot);
    const auto duplicates = model.around(0);
    assert(duplicates[0].token != duplicates[1].token);
    snapshot.entries[0].title = "Retagged";
    model.reconcile(snapshot);
    assert(model.around(0)[0].token == duplicates[0].token);
    assert(model.around(0)[1].token == duplicates[1].token);
    bool invalidOrdinal = false;
    try { QueueWindow::atOrdinal(duplicates, {}, 0); } catch (const std::out_of_range &) { invalidOrdinal = true; }
    assert(invalidOrdinal);
    puts("PASS queue window: ordinal/index identity, add/delete/move/clear, finite source barrier");
    // Exhaust all permutations of six slots around every audible anchor.
    std::vector<Token> tokens{1,2,3,4,5,6};
    do {
        for (Token anchor = 1; anchor <= 6; ++anchor)
            checkPlan({1,2,3,4,5,6}, tokens, anchor);
    } while (std::next_permutation(tokens.begin(), tokens.end()));
    checkPlan({1,2,3,4,5,6}, {3,4,7,8,9}, 4);
    checkPlan({}, {1,2,3}); checkPlan({1,2,3}, {});
    assert(QueueWindow::edits({1,2,3}, {1,2,3}, 2).empty());
    bool refused = false;
    try { QueueWindow::edits({1,2}, {2,3}, 1); } catch (const std::invalid_argument &) { refused = true; }
    assert(refused);
    using queue_audio::WindowEdits;
    using Parent = WindowEdits::Parent;
    std::vector<Parent> before{{1,{"one"}},{2,{"two","two-continuation"}},{3,{"three"}}};
    std::vector<Parent> wanted{{2,{"two","two-continuation"}},{4,{"four","four-continuation"}},{3,{"three"}}};
    WindowEdits::Snapshot queue{10,{"one","two","two-continuation","three"}};
    unsigned removes = 0, inserts = 0;
    bool foreign = false, updateLost = false;
    WindowEdits executor({
        [&] { return queue; },
        [&](uint32_t update, unsigned ordinal, unsigned count) {
            assert(update == queue.update);
            ++removes;
            queue.uris.erase(queue.uris.begin() + ordinal - 1, queue.uris.begin() + ordinal - 1 + count);
            if (!updateLost) ++queue.update;
        },
        [&](Token token, unsigned segment, unsigned ordinal) {
            ++inserts;
            const auto parent = std::find_if(wanted.begin(), wanted.end(), [&](const auto &p) { return p.token == token; });
            queue.uris.insert(queue.uris.begin() + ordinal - 1, parent->uris.at(segment));
            ++queue.update;
            if (foreign) queue.uris.push_back("foreign");
        }, {}});
    executor.apply(before, wanted, 2);
    assert(queue.uris == std::vector<std::string>({"two","two-continuation","four","four-continuation","three"}));
    assert(removes == 1 && inserts == 2);
    executor.apply(wanted, wanted, 2);
    assert(removes == 1 && inserts == 2);
    queue.uris.push_back("foreign");
    refused = false;
    try { executor.apply(wanted, wanted, 2); } catch (const std::runtime_error &) { refused = true; }
    assert(refused && removes == 1 && inserts == 2);
    queue = {10,{"one","two","two-continuation","three"}};
    foreign = true;
    refused = false;
    try { executor.apply(before, wanted, 2); } catch (const std::runtime_error &) { refused = true; }
    assert(refused && inserts == 3); // no second enqueue after the ownership race
    queue = {10,{"one","two","two-continuation","three"}};
    foreign = false; updateLost = true;
    refused = false;
    try { executor.apply(before, wanted, 2); } catch (const std::runtime_error &) { refused = true; }
    assert(refused && inserts == 3);
    queue = {10,{"one","two","two-continuation","three"}};
    updateLost = false;
    wanted = {{3,{"three"}}};
    executor.apply(before, wanted, 3);
    assert(queue.uris == std::vector<std::string>{"three"});
    executor.apply(wanted, {});
    assert(queue.uris.empty());
    puts("PASS queue window fencing: continuation blocks, fresh UpdateID, no-op ownership, foreign edit and unchanged revision stop edits");
    puts("PASS queue window edit plans: 4320 anchored permutations; append, trim and missing-anchor fence");
}
