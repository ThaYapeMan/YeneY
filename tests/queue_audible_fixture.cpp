// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_queue_sink.h"
#include "queue_controller.h"
#include "queue_lms_cli.h"
#include "third_party/yeney-core/core/player.h"
#include "upnp/http_server.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <thread>
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5)
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
    options.reanchor = [&](uint64_t id, uint32_t ms) { sink.reanchorSpeaker(id, ms); };
    options.audiblePosition = [&](uint64_t id, uint64_t frame, yeney::PlaybackState state) {
        sink.speakerPosition(id, frame, state);
    };
    std::atomic_uint64_t prepared{0};
    options.prepare = [&](uint64_t, uint32_t) {
        for (const auto &r : sink.snapshot())
            if (r.source.occurrence == prepared && r.item) {
                queue_audio::QueueAsset a;
                a.occurrence = prepared.load();
                a.item = r.item;
                a.metadata.title = "Title & " + std::to_string(prepared.load());
                a.metadata.artist = "Artist <one>";
                a.metadata.album = "Album & fixture";
                return a;
            }
        if (argc == 5 && prepared) {
            queue_audio::QueueAsset a;
            a.occurrence = prepared.load();
            return a; // decoder/refetch will supply the pending immutable item
        }
        throw std::runtime_error("fixture preparation missing");
    };
    if (argc == 5) {
        const auto port = unsigned(std::stoul(argv[4]));
        options.lmsControl = [port](const std::string &command) {
            return queue_audio::controlLms("127.0.0.1", "02:59:51:54:00:01", command, port);
        };
        options.fallbackRequest = [](const std::string &reason) {
            std::cerr << "FALLBACK " << reason << std::endl;
        };
    }
    auto controller = std::make_unique<queue_audio::QueueController>(options, server);
    std::atomic_bool consumeStart{false};
    sink.onQueueCommand([&](char command, uint32_t) {
        if (command == 's' && consumeStart) {
            consumeStart = false;
            controller->lmsCommand(1, command);
            return true;
        }
        if (command == 'p' || command == 'u' || command == 'q' || command == 'f') {
            controller->lmsCommand(1, command);
            return true;
        }
        return false;
    });
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
    bool playing = false, released = false, manual = false;
    auto sync = [&] {
        if (released || manual || (playing && !controller->status().active))
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
            if (line == "NEWRENDER") {
                sink.beginRender();
            } else if (line.rfind("READYITEM ", 0) == 0) {
                const auto id = std::stoull(line.substr(10));
                bool ready = false;
                for (const auto &r : sink.snapshot())
                    if (r.source.occurrence == id && r.decoded >= 0 && r.item && r.item->contentLength())
                        ready = true;
                std::cout << "ITEM " << ready << std::endl;
                continue;
            } else if (line.rfind("PLAN ", 0) == 0 || line.rfind("SELECT ", 0) == 0) {
                manual = true;
                std::istringstream input(line);
                std::string kind;
                uint64_t revision, current, id;
                input >> kind >> revision >> current;
                std::vector<queue_audio::QueueAsset> assets;
                auto records = sink.snapshot();
                while (input >> id) {
                    auto it = std::find_if(records.begin(), records.end(),
                                           [&](const auto &r) { return r.source.occurrence == id; });
                    if (it == records.end())
                        throw std::runtime_error("fixture unknown occurrence");
                    queue_audio::QueueAsset a;
                    a.occurrence = id;
                    a.item = it->item;
                    a.mixedIntro = it->source.transition == 1;
                    a.metadata.title = "Title & " + std::to_string(id);
                    a.metadata.artist = "Artist <one>";
                    a.metadata.album = "Album & fixture";
                    assets.push_back(a);
                }
                controller->lmsPlaylist(1, revision, current, assets, kind == "SELECT").get();
                consumeStart = kind == "SELECT";
            } else if (line.rfind("SUPPLY ", 0) == 0) {
                manual = true;
                std::istringstream input(line.substr(7));
                uint64_t supplied, token, id;
                uint32_t origin;
                input >> supplied >> token >> origin;
                std::vector<queue_audio::QueueAsset> assets;
                for (; input >> id;) {
                    for (const auto &r : sink.snapshot())
                        if (r.source.occurrence == id) {
                            queue_audio::QueueAsset a;
                            a.occurrence = id;
                            a.item = r.item;
                            if (id == supplied) {
                                a.token = token;
                                a.sourceOriginMs = origin;
                            }
                            a.metadata.title = "Title & " + std::to_string(id);
                            a.metadata.artist = "Artist <one>";
                            a.metadata.album = "Album & fixture";
                            assets.push_back(a);
                        }
                }
                controller->update(1, assets).get();
            } else if (line.rfind("COMMAND ", 0) == 0) {
                controller->lmsCommand(1, line.at(8)).get();
            } else if (line.rfind("SEEK ", 0) == 0) {
                controller->lmsSeek(1, std::stoul(line.substr(5))).get();
                if (argc == 5)
                    consumeStart = true; // retained CLI refetch, host identity confirmed

            } else if (line.rfind("PREPARED ", 0) == 0) {
                prepared = std::stoull(line.substr(9));
            } else if (line.rfind("EVICT ", 0) == 0) {
                auto id = std::stoull(line.substr(6));
                for (const auto &r : sink.snapshot())
                    if (r.source.occurrence == id)
                        r.item->cancel("item history evicted; LMS re-render required");
            } else if (line == "RELEASE") {
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
    done = true;
    core.join(); // no command callback may outlive the controller
    controller.reset();
    server->shutdown();
    std::cout << "BYE" << std::endl;
}
