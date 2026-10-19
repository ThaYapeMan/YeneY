// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_controller.h"
#ifdef MODE_FIXTURE
#include "queue_lms_cli.h"
#include "room_mode.h"
#include <mutex>
#endif
#include "upnp/http_server.h"
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
using namespace queue_audio;
int main(int argc, char **argv) {
    if (argc != 3
#ifdef MODE_FIXTURE
        && argc != 4
#endif
    )
        return 2;
    (void)upnp::titleFormat();
    Config storage;
    storage.root = argv[1];
    storage.ramBytes = 65536;
    Store store("Study", storage);
    auto server = std::make_shared<upnp::HttpServer>(0);
    ControllerOptions options;
    options.room = {"127.0.0.1", "RINCON_TEST", "Study", "Study", {"Study"}};
    options.room.location = "http://127.0.0.1:" + std::string(argv[2]) + "/description.xml";
    options.session = "controller-test-session";
    options.httpBase = "http://127.0.0.1:" + std::to_string(server->port());
    options.backupTitle = [] { return "Before YeneY 2026-10-19 12:34"; };
    options.log = [](const std::string &s) { std::cerr << s << std::endl; };
#ifdef MODE_FIXTURE
    std::mutex evidenceMutex;
    ConfirmedItem latest;
    options.managedFallback = true;
    options.audiblePosition = [](auto, auto, auto) {};
    options.confirmed = [&](const ConfirmedItem &e) {
        std::lock_guard<std::mutex> l(evidenceMutex);
        latest = e;
    };
    std::string requested;
    options.fallbackRequest = [&](const std::string &r) {
        std::lock_guard<std::mutex> l(evidenceMutex);
        requested = r;
    };
    options.lmsControl = [](const auto &) { return true; };
#endif
    auto controller = std::make_unique<QueueController>(options, server);
#ifdef MODE_FIXTURE
    unsigned releases = 0, switches = 0;
    bridge::RoomMode mode(
        "Study", "Study", bridge::selectPlaybackMode("queue", options.room, true),
        {[&](const auto &e) { controller->freeze(e.epoch).get(); },
         [&](const auto &) {
             ++releases;
             controller->release("mode-handoff").get();
         },
         [&](uint64_t, const auto &e) {
             ++switches;
             unsigned index = e.index + (e.ended && e.frame >= e.promised ? 1 : 0);
             if (!controlLms("127.0.0.1", "02:59:51:54:00:01", "playlist index " + std::to_string(index),
                             std::stoul(argv[3])))
                 throw std::runtime_error("CLI index");
             if (!e.ended && !controlLms("127.0.0.1", "02:59:51:54:00:01",
                                         "time " + std::to_string(e.sourceMs / 1000), std::stoul(argv[3])))
                 throw std::runtime_error("CLI time");
         },
         options.log});
    mode.newPlay(true, 1, 100);
#endif
    std::map<uint64_t, QueueAsset> assets;
    auto append = [&](uint64_t id, uint64_t count) {
        int32_t value = int32_t((0x123400 + id) << 8);
        std::vector<yeney::Frame> pcm(count, {value, -value});
        assets.at(id).item->append(pcm.data(), pcm.size(), false);
    };
    std::cout << "READY " << server->port() << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            std::istringstream input(line);
            std::string action;
            input >> action;
            uint64_t id = 0, epoch = 0;
#ifdef MODE_FIXTURE
            if (action == "MODE") {
                std::cout << "MODE " << bridge::RoomMode::json(mode.status()) << " " << releases << " "
                          << switches << std::endl;
                continue;
            } else if (action == "REQUEST") {
                std::string r;
                uint64_t now;
                input >> r >> now;
                mode.request(r, now);
                if (mode.status().active == "queue")
                    controller->drain(1).get();
            } else if (action == "EVIDENCE") {
                uint64_t now;
                input >> now;
                ConfirmedItem e;
                std::string r;
                {
                    std::lock_guard<std::mutex> l(evidenceMutex);
                    e = latest;
                    r = requested;
                    requested.clear();
                }
                bridge::ModeEvidence v;
                v.generation = mode.status().generation;
                v.epoch = e.epoch;
                v.occurrence = e.occurrence;
                v.token = e.occurrence;
                v.index = 4;
                v.frame = e.frame;
                v.promised = e.promised;
                v.actual = e.actual;
                v.sourceMs = e.sourceMs;
                v.rate = e.rate;
                v.owned = true;
                v.playing = e.state == yeney::PlaybackState::Playing;
                v.ended = e.state == yeney::PlaybackState::Stopped || e.frame >= e.promised;
                mode.evidence(v, now);
                if (!r.empty())
                    mode.request(r, now);
            } else if (action == "AUDIBLE") {
                uint64_t g, now;
                input >> g >> now;
                mode.streamAudible(g, now, 48000);
            } else if (action == "LATE") {
                upnp::GenaEvent e;
                controller->notify(1, e).get();
            } else
#endif
                if (action == "PUT") {
                char kind;
                uint64_t frames, produced;
                unsigned rate;
                if (!(input >> id >> kind >> frames >> produced >> rate) ||
                    (assets.count(id) && assets.at(id).item))
                    throw std::runtime_error("bad PUT");
                QueueAsset a;
                a.occurrence = id;
                a.metadata.title = "Title & " + std::to_string(id);
                a.metadata.artist = "Artist <one>";
                a.metadata.album = "Album & fixture";
                a.metadata.artwork = "http://127.0.0.1:9000/music/" + std::to_string(id) + "/cover.jpg";
                if (kind != 'N')
                    a.item = store.create({std::to_string(id), rate,
                                           kind == 'P' ? std::optional<uint64_t>(frames) : std::nullopt, 1});
                assets.insert_or_assign(id, std::move(a));
                if (produced)
                    append(id, produced);
            } else if (action == "APPEND") {
                uint64_t count;
                input >> id >> count;
                append(id, count);
            } else if (action == "FINISH") {
                input >> id;
                assets.at(id).item->finish();
            } else if (action == "PLAY" || action == "UPDATE") {
                uint64_t current = 0;
                input >> epoch;
                if (action == "PLAY")
                    input >> current;
                std::vector<QueueAsset> plan;
                while (input >> id)
                    plan.push_back(assets.at(id));
                if (action == "PLAY")
                    controller->play(epoch, current, std::move(plan)).get();
                else
                    controller->update(epoch, std::move(plan)).get();
            } else if (action == "RELEASE") {
                std::string reason;
                input >> reason;
                controller->release(reason).get();
            } else if (action == "STATUS") {
                auto status = controller->status();
                std::cout << "STATUS " << status.active << ' ' << status.epoch << ' ' << status.mirrored
                          << ' ' << std::quoted(status.reason) << ' ' << std::quoted(status.fault)
                          << std::endl;
                continue;
            } else if (action == "DUPLICATE" || action == "GROUP") {
                auto second = options;
                if (action == "GROUP")
                    second.room.members.push_back("Other");
                QueueController rejected(second, server);
                throw std::runtime_error("unexpected second controller");
            } else if (action == "EXIT") {
                break;
            } else
                throw std::runtime_error("bad command");
            std::cout << "OK" << std::endl;
        } catch (const std::exception &error) {
            std::cout << "ERR " << error.what() << std::endl;
        }
    }
    controller.reset();
    server->shutdown();
    std::cout << "BYE" << std::endl;
}
