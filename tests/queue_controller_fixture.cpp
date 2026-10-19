// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_controller.h"
#include "upnp/http_server.h"
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
using namespace queue_audio;
int main(int argc, char **argv) {
    if (argc != 3)
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
    auto controller = std::make_unique<QueueController>(options, server);
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
