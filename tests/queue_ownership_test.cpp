// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "upnp/queue_ownership.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
int main() {
    upnp::QueueSnapshot queue{1, {"foreign-one", "foreign-two"}};
    std::vector<upnp::SavedQueue> saved{{"unrelated", "Keep me", 0}};
    for (unsigned i = 0; i < 6; ++i)
        saved.push_back({std::to_string(i), "Before YeneY old", i});
    bool crossfade = true, verify = true;
    unsigned saves = 0, removes = 0;
    upnp::QueueOwnershipIO io;
    io.browse = [&] { return queue; };
    io.playlists = [&] { return saved; };
    io.save = [&](const std::string &title) {
        ++saves;
        if (verify)
            saved.push_back({"new", title, 10});
        return "new";
    };
    io.deletePlaylist = [&](const std::string &id) {
        assert(id != "unrelated");
        saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const auto &p) { return p.id == id; }),
                    saved.end());
    };
    io.crossfade = [&] { return crossfade; };
    io.setCrossfade = [&](bool enabled) { crossfade = enabled; };
    io.log = [](const std::string &) {};
    io.remove = [&](uint32_t update, unsigned first, unsigned count) {
        assert(update == queue.update);
        ++removes;
        queue.uris.erase(queue.uris.begin() + first - 1, queue.uris.begin() + first - 1 + count);
        ++queue.update;
    };
    upnp::QueueOwnership lease(io, "session");
    lease.acquire("Before YeneY 2026-10-18 12:34");
    assert(lease.active() && !crossfade && queue.uris.empty() && saves == 1 && removes == 1 &&
           saved.size() == 6);
    const std::string own = "http://bridge/music/queue.wav?session=session&epoch=1&item=2&revision=1";
    queue.uris = {own, "foreign-new", own,
                  "http://bridge/music/queue.wav?session=session-other&epoch=1&item=2&revision=1"};
    ++queue.update;
    lease.release();
    assert(!lease.active() && crossfade);
    assert(queue.uris.size() == 2 && queue.uris[0] == "foreign-new" &&
           queue.uris[1].find("session-other") != std::string::npos);
    verify = false;
    upnp::QueueOwnership failing(io, "another");
    auto before = queue.uris;
    bool failed = false;
    try {
        failing.acquire("Before YeneY 2026-10-18 12:35");
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed && queue.uris == before && crossfade);
    verify = true;
    auto normalRemove = io.remove;
    io.remove = [](uint32_t, unsigned, unsigned) { throw std::runtime_error("stale-update"); };
    upnp::QueueOwnership conflict(io, "conflict");
    failed = false;
    try {
        conflict.acquire("Before YeneY 2026-10-18 12:36");
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed && !crossfade && queue.uris == before);
    conflict.release();
    assert(crossfade && queue.uris == before);
    io.remove = normalRemove;
    // Release commits local state before I/O, logs once, never retries after refusal.
    for (const auto &error : {"HTTP=500", "timeout", "fault=712", "fault=701"}) {
        queue.uris.clear();
        bool failRestore = false;
        unsigned logs = 0, writes = 0;
        upnp::QueueOwnership *releasing = nullptr;
        io.stop = [&] { assert(releasing && !releasing->active()); };
        io.log = [&](const auto &) { ++logs; };
        io.setCrossfade = [&](bool value) {
            ++writes;
            if (failRestore) {
                assert(releasing && !releasing->active());
                throw std::runtime_error(error);
            }
            crossfade = value;
        };
        upnp::QueueOwnership retry(io, "retry");
        retry.acquire("Before YeneY retry");
        releasing = &retry;
        failRestore = true;
        retry.release(true, {}, true);
        assert(!retry.active() && logs == 1);
        retry.release();
        assert(logs == 1 && writes == 2);
        failRestore = false;
        retry.acquire("Before YeneY retry");
        assert(retry.active());
        retry.release(false);
        assert(!retry.active() && writes == 3);
    }
    std::cout << "PASS queue ownership: verified backup before mutation, five-prefix retention, foreign "
                 "entry preservation, crossfade restore, failed save leaves queue intact\n";
}
