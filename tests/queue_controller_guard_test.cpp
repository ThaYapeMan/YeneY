// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_controller.h"
#include "upnp/http_server.h"
#include <cassert>
#include <iostream>
int main() {
    auto server = std::make_shared<upnp::HttpServer>(0);
    queue_audio::ControllerOptions options;
    options.room = {"127.0.0.1", "RINCON_TEST", "Study", "Study", {"Study"}};
    bool guarded = false;
    try {
        queue_audio::QueueController controller(options, server);
    } catch (const std::runtime_error &e) {
        guarded = std::string(e.what()) == "queue-unavailable";
    }
    assert(guarded && server->resource("queue").uri.empty());
    std::cout
        << "PASS queue controller production guard: no controller or finite route activation before part 7\n";
}
