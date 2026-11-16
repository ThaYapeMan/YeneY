// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Exercise the transport timers with an explicit clock, without changing production intervals.
#include "../speaker_websocket.cpp"
#include <cassert>
int main(int argc, char **argv) {
    assert(argc == 3);
    bridge::LocalSession session;
    session.open(argv[1], [](const std::string &line) { std::cout << line << std::endl; });
    const std::string mode = argv[2];
    if (mode == "renew") {
        session.maintenance(bridge::positionMonotonic()+86401);
        const auto end = bridge::positionMonotonic()+.2;
        while (bridge::positionMonotonic()<end) session.message();
        std::cout << "PASS renew before three days at 24 hours" << std::endl;
    } else {
        const auto now = bridge::positionMonotonic();
        session.connection.maintenance(now+31);
        const auto end = bridge::positionMonotonic()+.2;
        while (bridge::positionMonotonic()<end) session.message();
        if (mode == "pong") {
            session.connection.maintenance(now+35);
            std::cout << "PASS matching pong keeps transport alive" << std::endl;
        } else {
            try { session.connection.maintenance(now+35); }
            catch (const std::exception &e) {
                assert(std::string(e.what()) == "WebSocket pong missing for 3 s");
                std::cout << "PASS missing pong fails at its own three-second deadline" << std::endl;
                return 0;
            }
            assert(false);
        }
    }
}
