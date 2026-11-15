// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_native_decoder.h"
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    yeney::DecoderConfig config; config.codec = 'l'; config.finitePcmContainers = true;
    auto decoder = bridge::nativeQueueDecoder(config, "127.0.0.1", "13", 0, {}, std::stoul(argv[1]));
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    uint8_t credits[65536]{};
    while (std::chrono::steady_clock::now() < deadline) {
        decoder->feed(credits, sizeof credits);
        yeney::Frame first;
        if (decoder->take(&first, 1)) {
            uint64_t origin = 99;
            if (!decoder->sourceStart(origin) || origin) return 4;
            std::cout << "FIRST " << first.left << ' ' << first.right << std::endl;
            return 0;
        }
        if (!decoder->error().empty()) { std::cerr << decoder->error(); return 5; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 3;
}
