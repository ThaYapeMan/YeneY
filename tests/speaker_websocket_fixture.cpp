// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_websocket.h"
#include <chrono>
#include <iostream>
#include <thread>
int main() {
    bridge::WebsocketSpeakerPosition source("127.0.0.1");
    std::cout.setf(std::ios::unitbuf);
    for (int n = 0; n < 5000; ++n) {
        const double now = bridge::positionMonotonic();
        auto value = source.position(now);
        std::cout.precision(12);
        std::cout << now << '\t' << (value ? value->positionMs : -1) << '\t'
                  << (value ? value->item : "") << '\t' << source.failure() << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
