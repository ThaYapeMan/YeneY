// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_websocket.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
volatile std::sig_atomic_t regroup = 0;
void group(int) { regroup = 1; }
int main() {
    std::signal(SIGUSR1, group);
    const auto port = std::getenv("YENEY_WS_TEST_PORT");
    bridge::WebsocketSpeakerPosition source(port ? "127.0.0.1:" + std::string(port) : "127.0.0.1");
    std::cout.setf(std::ios::unitbuf);
    for (int n = 0; n < 5000; ++n) {
        if (regroup) { regroup = 0; source.groupChanged(); }
        for (const auto &line : source.takeJournal()) std::cerr << line << std::endl;
        const double now = bridge::positionMonotonic();
        auto value = source.position(now);
        std::cout.precision(12);
        std::cout << now << '\t' << (value ? value->positionMs : -1) << '\t'
                  << (value ? value->item : "") << '\t' << source.failure() << '\t' << (value ? value->state : "") << '\t'
                  << source.revision() << '\t' << source.rawState() << '\t' << source.quality() << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
