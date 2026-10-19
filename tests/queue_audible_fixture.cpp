// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include "queue_controller.h"
#include "third_party/yeney-core/core/player.h"
#include "upnp/http_server.h"
#include <atomic>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <thread>
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    (void)upnp::titleFormat();
    queue_audio::Config storage;
    storage.root = argv[1];
    queue_audio::CoreQueueSink sink("Study", storage, 60);
    sink.useSpeakerClock();
    auto server = std::make_shared<upnp::HttpServer>(0);
    queue_audio::ControllerOptions options;
    options.room = {"127.0.0.1", "RINCON_TEST", "Study", "Study", {"Study"}};
    options.room.location = "http://127.0.0.1:" + std::string(argv[2]) + "/description.xml";
    options.session = "audible-test-session";
    options.httpBase = "http://127.0.0.1:" + std::to_string(server->port());
    options.log = [](const std::string &s) { std::cerr << s << std::endl; };
    options.audiblePosition = [&](uint64_t id, uint64_t frame, yeney::PlaybackState state) {
        sink.speakerPosition(id, frame, state);
    };
    auto controller = std::make_unique<queue_audio::QueueController>(options, server);
    std::mutex eventMutex;
    upnp::GenaEvent lastEvent;
    server->setNotifyHandler([&](const upnp::GenaEvent &event) {
        // The production subscription owner validates the SID and coordinator.
        if (event.sid != "uuid:audible-fixture")
            return false;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            lastEvent = event;
        }
        controller->notify(1, event);
        return true;
    });
    yeney::Config config;
    config.name = "Queue fixture";
    config.server = "127.0.0.1";
    config.port = std::stoul(argv[3]);
    config.futurePcmSeconds = 60;
    config.log = options.log;
    std::atomic_bool done{false};
    yeney::Player player(config, sink);
    std::thread core([&] { player.run(done); });
    bool playing = false, released = false;
    auto sync = [&] {
        if (released)
            return;
        std::vector<queue_audio::QueueAsset> plan;
        for (const auto &r : sink.snapshot()) {
            if (!r.item || !r.item->contentLength() || r.source.occurrence >= UINT64_MAX / 2)
                break;
            queue_audio::QueueAsset a;
            a.occurrence = r.source.occurrence;
            a.item = r.item;
            a.strmAt = r.strmAt;
            a.metadata.title = "Title & " + std::to_string(a.occurrence);
            a.metadata.artist = "Artist <one>";
            a.metadata.album = "Album & fixture";
            plan.push_back(std::move(a));
        }
        if (plan.empty())
            return;
        if (!playing) {
            controller->play(1, plan.front().occurrence, plan).get();
            playing = true;
        } else
            controller->update(1, plan).get();
    };
    std::cout << "READY " << server->port() << std::endl;
    try {
        for (;;) {
            sync();
            pollfd input{0, POLLIN, 0};
            if (poll(&input, 1, 20) <= 0)
                continue;
            std::string line;
            if (!std::getline(std::cin, line) || line == "EXIT")
                break;
            if (line == "RELEASE") {
                released = true;
                controller->release().get();
            } else if (line == "STALE") {
                upnp::GenaEvent event;
                {
                    std::lock_guard<std::mutex> lock(eventMutex);
                    event = lastEvent;
                }
                controller->notify(0, event).get();
            } else if (line != "SYNC")
                throw std::runtime_error("bad command");
            std::cout << "OK" << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        done = true;
    }
    server->setNotifyHandler({});
    controller.reset();
    done = true;
    core.join();
    server->shutdown();
    std::cout << "BYE" << std::endl;
}
