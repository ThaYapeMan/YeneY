// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mode_sink.h"
#include "queue_playlist.h"
#include "room_mode.h"
#include "stream_metrics.h"
#include <cassert>
#include <fstream>
#include <iostream>
struct Sink : yeney::Sink {
    unsigned writes = 0, flushed = 0, boundaries = 0;
    bool queue;
    explicit Sink(bool q) : queue(q) {}
    bool decodeAhead() const override { return queue; }
    bool speakerDriven() const override { return queue; }
    void trackBoundary(uint64_t, const yeney::Format &, bool) override { ++boundaries; }
    size_t write(const yeney::Frame *, size_t n) override {
        writes += n;
        return n;
    }
    void pause() override {}
    void resume() override {}
    void stop() override {}
    void flush() override { ++flushed; }
    uint64_t audibleFrames() const override { return 0; }
    bool paced() const override { return false; }
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override {}
};
int main() {
    upnp::Speaker room{"127.0.0.1", "RINCON_TEST", "Study", "Study", {"Study"}};
    {
        unsigned releases = 0, switches = 0, requests = 0;
        std::vector<std::string> logs;
        bridge::RoomMode mode("Study", "Study", bridge::selectPlaybackMode("queue", room, true),
                              {{},
                               [&](const auto &) { ++releases; },
                               [&](auto, const auto &) { ++switches; },
                               [&](const std::string &v) { logs.push_back(v); }});
        mode.onRearmLoop([&] {
            ++requests;
            mode.request("rearm-loop", 400);
        });
        assert(mode.newPlay(true, 1, 0));
        auto initial = mode.status().generation;
        assert(!mode.newPlay(true, 2, 100)); // repeated s without explicit stop
        assert(mode.status().generation == initial);
        for (unsigned i = 0; i < 40; ++i) {
            mode.command('q');
            mode.stop();
            mode.command('s');
            mode.adoptPlay(); // LMS jump/start does not reserve a new generation
            assert(mode.status().generation == initial && !mode.status().latched);
        }
        for (unsigned i = 1; i <= 3; ++i) {
            mode.command('q');
            mode.stop();
            mode.command('s');
            assert(mode.newPlay(true, i + 1, i * 100));
        }
        mode.command('q');
        mode.stop();
        mode.command('s');
        assert(!mode.newPlay(true, 5, 400));
        assert(mode.status().lastFallback == "rearm-loop" && mode.status().latched);
        assert(releases == 1 && switches == 1 && requests == 1);
        for (unsigned i = 0; i < 40; ++i) {
            mode.stop();
            assert(!mode.newPlay(true, 6 + i, 500 + i));
            mode.tick(600 + i);
        }
        assert(releases == 1 && switches == 1 && requests == 1);
        assert(std::count_if(logs.begin(), logs.end(), [](const auto &s) {
                   return s.find("rearm-loop room=") != std::string::npos &&
                          s.find("last_commands=qsqsqsqs") != std::string::npos;
               }) == 1);
        std::cout << "PASS rearm-loop guard: repeated start preserves generation; fourth rearm in 5 s logs "
                     "last commands, one latched release/fallback, forty retries suppressed\n";
    }
    {
        bridge::RoomMode mode("Study", "Study", bridge::selectPlaybackMode("queue", room, true), {});
        assert(mode.newPlay(true, 1, 0));
        for (unsigned i = 1; i <= 6; ++i) {
            mode.stop();
            assert(mode.newPlay(true, i + 1, i * 6000));
        }
        assert(!mode.status().latched);
        std::cout << "PASS rearm-loop window: legitimate stop/new-play cycles outside five seconds remain "
                     "eligible\n";
    }
    for (std::string reason : {"foreign-control", "preparation-timeout", "soap-error", "length-fault",
                               "storage-fault", "coordinator-change", "lms-unavailable", "rearm-loop"}) {
        unsigned stops = 0, releases = 0, switches = 0;
        uint64_t streamGeneration = 0;
        bridge::ModeEvidence continued;
        bridge::RoomMode mode("Study", "Study", bridge::selectPlaybackMode("queue", room, true),
                              {[&](const auto &) { ++stops; }, [&](const auto &) { ++releases; },
                               [&](uint64_t g, const auto &e) {
                                   ++switches;
                                   streamGeneration = g;
                                   continued = e;
                               },
                               [](const auto &) {}});
        assert(mode.newPlay(true, 7, 100));
        auto g = mode.status().generation;
        bridge::ModeEvidence e;
        e.generation = g;
        e.epoch = 7;
        e.occurrence = 19;
        e.token = 91;
        e.index = 4;
        e.rate = 44100;
        e.promised = e.actual = 441000;
        e.frame = 88200;
        e.sourceMs = 2000;
        e.owned = e.playing = true;
        if (reason == "length-fault" || reason == "storage-fault")
            e.usable = false;
        mode.evidence(e, 200);
        mode.request(reason, 300);
        mode.request(reason, 301);
        const bool cut = reason == "foreign-control" || reason == "coordinator-change" || !e.usable;
        if (!cut) {
            assert(switches == 0);
            e.frame = e.promised;
            mode.evidence(e, 500);
            assert(switches == 0);
            e.ended = true;
            e.sourceMs = 10000;
            mode.evidence(e, 600);
        }
        assert(switches == 1 && releases == 1 && stops == unsigned(cut));
        assert(continued.index == 4 && continued.token == 91 && continued.sourceMs == (cut ? 2000 : 10000));
        assert(mode.status().state == "handoff" && mode.status().gapMs < 0);
        assert(!mode.accepts(g, true));
        mode.evidence(e, 650);
        mode.streamAudible(g, 700, 48000);
        assert(mode.status().state == "handoff");
        mode.streamAudible(streamGeneration, 900, 48000);
        assert(mode.status().state == "active" && mode.status().gapMs == (cut ? 600 : 300));
        mode.request(reason, 1000);
        assert(switches == 1);
        assert(!mode.newPlay(true, 8, 1000));
        mode.stop();
        assert(!mode.newPlay(false, 8, 1001));
        assert(!mode.newPlay(true, 8, 1002));
        mode.stop();
        assert(mode.newPlay(true, 8, 1100));
        assert(!mode.status().latched);
        mode.evidence(e, 1200); // old session evidence cannot repopulate the new epoch
        assert(mode.status().item.epoch == 8 && mode.status().item.occurrence == 0);
        std::cout << "PASS mode " << reason
                  << ": one fallback/release, preserved confirmed tail/point, same LMS token/index/time, "
                     "measured gap, stale evidence fenced, stop/new-play readiness latch\n";
    }
    bridge::RoomMode timeout("Study", "Study", bridge::selectPlaybackMode("queue", room, true), {});
    timeout.newPlay(true, 1, 1);
    timeout.tick(15001);
    assert(timeout.status().lastFallback == "preparation-timeout");
    unsigned stopAttempts = 0, switched = 0;
    bridge::RoomMode unconfirmed("Study", "Study", bridge::selectPlaybackMode("queue", room, true),
                                 {[&](const auto &) {
                                      ++stopAttempts;
                                      throw std::runtime_error("speaker-unreachable");
                                  },
                                  {},
                                  [&](auto, const auto &) { ++switched; },
                                  {}});
    unconfirmed.newPlay(true, 1, 1);
    bridge::ModeEvidence pending;
    pending.generation = unconfirmed.status().generation;
    pending.epoch = 1;
    pending.occurrence = 1;
    pending.owned = pending.playing = true;
    unconfirmed.evidence(pending, 100);
    unconfirmed.request("foreign-control", 200);
    unconfirmed.tick(100000);
    assert(stopAttempts == 1 && switched == 0);
    pending.ended = true;
    unconfirmed.evidence(pending, 100001);
    assert(stopAttempts == 1 && switched == 1);
    std::cout << "PASS unconfirmed stop: no early switch, no repeated Stop; confirmed end releases the "
                 "waiting handoff\n";
    bridge::RoomMode idle("Study", "Study", bridge::selectPlaybackMode("queue", room, true), {});
    idle.newPlay(true, 1, 0);
    idle.tick(1000000);
    assert(!idle.status().latched);
    idle.beginPreparation(1000000);
    idle.tick(1015000);
    assert(idle.status().lastFallback == "preparation-timeout");
    std::cout << "PASS idle queue selection: preparation deadline starts on LMS play, not service startup\n";
    Sink q(true), s(false);
    bridge::ModeSink router(q, s);
    yeney::Frame f{};
    assert(router.write(&f, 1) == 1);
    router.select(false, 2);
    assert(router.write(&f, 1) == 0);
    router.trackBoundary(0, {}, false);
    assert(s.boundaries == 0);
    router.playbackReady(2);
    router.trackBoundary(0, {}, false);
    assert(s.boundaries == 1);
    router.playbackReady(1); // stale readiness cannot reopen the old generation
    assert(router.write(&f, 1) == 0);
    router.admit();
    assert(router.write(&f, 1) == 1);
    assert(q.writes == 1 && s.writes == 1);
    router.select(true, 3);
    assert(router.write(&f, 1) == 0);
    router.playbackReady(2);
    router.admit();
    assert(router.write(&f, 1) == 0);
    router.playbackReady(3);
    assert(router.write(&f, 1) == 1);
    assert(q.writes == 2 && s.writes == 1);
    std::cout << "PASS mode sink generation: old PCM blocked before new core epoch, stream writes admitted "
                 "only after reset, no old queue callbacks\n";
    queue_audio::PlaylistReader reader;
    auto a = reader.parse("mac status playlist_tracks:3 playlist_cur_index:1 time:2.5 playlist%20index:0 "
                          "id:10 title:One%20song playlist%20index:1 id:20 playlist%20index:2 id:30");
    auto b = reader.parse("mac status playlist_tracks:3 playlist_cur_index:0 time:0 playlist%20index:0 id:20 "
                          "playlist%20index:1 id:30 playlist%20index:2 id:10");
    assert(a.entries[1].token == b.entries[0].token && a.entries[0].title == "One song");
    assert(a.timeMs == 2500 && b.revision > a.revision);
    auto empty = reader.parse("playlist_tracks:0 playlist_cur_index:-1 time:0");
    assert(empty.entries.empty());
    queue_audio::PlaylistReader duplicate;
    duplicate.parse(
        "playlist_tracks:2 playlist_cur_index:0 playlist%20index:0 id:10 playlist%20index:1 id:10");
    bool ambiguous = false;
    try {
        duplicate.parse("playlist_tracks:1 playlist_cur_index:0 playlist%20index:0 id:10");
    } catch (const std::exception &) {
        ambiguous = true;
    }
    assert(ambiguous);
    std::cout << "PASS production playlist snapshots: decoded fields, full count/index validation, stable "
                 "occurrence tokens across move, LMS source-time origin\n";
    bridge::RoomMode status("Study", "Study", bridge::selectPlaybackMode("queue", room, false), {});
    bridge::setRoomMode(&status);
    bridge::StreamMetrics metrics;
    metrics.configure("mode-fixture", "/tmp");
    metrics.tick(60000);
    std::ifstream json("/tmp/stream-pacing-" + bridge::StreamMetrics::hex("mode-fixture") + ".json");
    std::string line;
    getline(json, line);
    for (auto field : {"requested", "active", "reason", "occurrence", "epoch", "owned", "mirrored_ahead",
                       "last_fallback", "handoff_gap_ms"})
        assert(line.find(field) != std::string::npos);
    assert(line.find("queue-unavailable") != std::string::npos);
    bridge::setRoomMode(nullptr);
    std::cout << "PASS room status JSON: requested/active mode, reason, occurrence/epoch, ownership, ahead, "
                 "fallback and gap; closed guard remains queue-unavailable\n";
}
