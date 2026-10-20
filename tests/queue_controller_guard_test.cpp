// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_controller.h"
#include "upnp/http_server.h"
#include <cassert>
#include <iostream>
int main() {
    auto server = std::make_shared<upnp::HttpServer>(0);
    queue_audio::ControllerOptions options;
    options.room = {"127.0.0.1", "RINCON_TEST", "Study", "Study", {"Study"}};
    options.session = "production-admission-test";
    options.httpBase = "http://127.0.0.1:" + std::to_string(server->port());
    {
        queue_audio::QueueController controller(options, server);
        assert(!controller.status().active);
    }
    options.room.members.push_back("Other");
    bool grouped = false;
    try {
        queue_audio::QueueController controller(options, server);
    } catch (const std::invalid_argument &e) {
        grouped = std::string(e.what()) == "group-rate-undecided";
    }
    assert(grouped);
    std::cout << "PASS queue controller production admission: standalone "
                 "controller available, grouped room refused before takeover\n";
}
