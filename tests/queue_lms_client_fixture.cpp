// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_lms_client.h"
#include "room_mode.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
using namespace queue_audio;
int main(int argc, char **argv) {
    assert(argc == 3);
    const std::string profile = argv[2];
    std::atomic<uint64_t> clock{100}, slow{0}, failed{0}, fallbacks{0};
    std::atomic<bool> done{false};
    bridge::PlaybackSelection selection;
    selection.requested = "queue";
    selection.selected = bridge::PlaybackMode::Queue;
    bridge::RoomMode mode("Study", "Study", selection, {});
    mode.newPlay(true, 1, 100);
    LmsClient::Options o;
    o.port = std::stoul(argv[1]);
    o.metrics = [&](uint64_t s, uint64_t f) {
        slow = s;
        failed = f;
        mode.lmsMetrics(s, f);
    };
    o.log = [](const std::string &v) { std::cout << v << std::endl; };
    o.unavailable = [&](const std::string &reason) {
        assert(reason == "lms-unavailable");
        ++fallbacks;
        mode.request(reason, clock);
    };
    if (profile == "outage") {
        o.clock = [&] { return clock.load(); };
        o.queryMs = 30;
    }
    if (profile == "late")
        o.queryMs = 300;
    LmsClient cli("127.0.0.1", "aa:bb:cc:dd:ee:ff", o);
    std::string reply;
    if (profile == "guard") {
        assert(!cli.submit({"playlist index 0"}, "readiness"));
        assert(!cli.submit({"play"}, ""));
        assert(!cli.submit({"playlist clear"}, "sonos-user-action"));
        assert(!cli.query("playlist index 1", reply));
        assert(cli.counts() == std::make_pair(uint64_t(0), uint64_t(0)));
        std::cout << "PASS queue CLI mutation choke: missing/internal reason and query bypass rejected "
                     "before any network write\n";
    } else if (profile == "actions") {
        const auto start = std::chrono::steady_clock::now();
        assert(cli.submit({"pause 1", "playlist index 2", "time 20"}, "sonos-user-action",
                          [&] { done = true; }));
        assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50));
        for (int i = 0; i < 1500 && !done; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(done && !fallbacks && slow >= 3);
    } else if (profile == "snapshot" || profile == "snapshot20") {
        auto at = std::chrono::steady_clock::now();
        assert(cli.query("status 0 60000 tags:alKu", reply));
        assert(!fallbacks && slow >= 1);
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - at)
                .count();
        mode.deferPreparation(elapsed);
        mode.tick(100 + elapsed + 1);
        assert(mode.status().lastFallback.empty());
    } else if (profile == "late") {
        assert(cli.query("mode ?", reply));
        assert(reply.find("pause") != std::string::npos);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        assert(cli.query("playlist index ?", reply));
        assert(reply.back() == '2');
        assert(failed == 1 && !fallbacks);
    } else if (profile == "outage") {
        assert(!cli.query("mode ?", reply));
        assert(failed == 2 && !fallbacks);
        clock = 30099;
        cli.tick();
        assert(!fallbacks);
        clock = 30100;
        cli.tick();
        for (int i = 0; i < 100 && !fallbacks; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(fallbacks == 1);
        cli.tick();
        assert(fallbacks == 1);
    } else if (profile == "mutation-drop") {
        assert(cli.submit({"pause 1"}, "sonos-user-action", [&] { done = true; }));
        for (int i = 0; i < 500 && !done; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(done && failed == 1 && !fallbacks);
    } else if (profile == "drop") {
        assert(cli.query("mode ?", reply));
        assert(failed == 1 && !fallbacks);
    } else
        return 2;
    auto status = mode.status();
    std::cout << "STATUS " << bridge::RoomMode::json(status) << std::endl;
    cli.shutdown();
}
