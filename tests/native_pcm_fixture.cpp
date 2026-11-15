// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "queue_native_decoder.h"
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    yeney::DecoderConfig config; config.finitePcmContainers = true; config.endian = argv[2][0];
    auto decoder = bridge::nativeQueueDecoder(config, "127.0.0.1", "15", 0, {}, std::stoul(argv[1]));
    uint64_t count = 0, digest = 1469598103934665603ull;
    bool announced = false;
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    uint8_t credits[65536]{};
    while (!decoder->done()) {
        if (std::chrono::steady_clock::now() > deadline) return 3;
        decoder->feed(credits, sizeof credits);
        yeney::Frame frames[256]; auto n = decoder->take(frames, 256);
        if (n && !announced) {
            const auto length = decoder->exactFrames();
            std::cout << "EARLY " << (length ? std::to_string(*length) : "unknown") << std::endl;
            announced = true;
        }
        for (size_t i = 0; i < n; ++i) for (uint32_t value : {uint32_t(frames[i].left),uint32_t(frames[i].right)})
            for (unsigned shift : {0,8,16,24}) digest = (digest ^ uint8_t(value >> shift))*1099511628211ull;
        count += n;
        if (!decoder->error().empty()) { std::cerr << decoder->error(); return 4; }
        if (!n) std::this_thread::yield();
    }
    std::cout << "FINAL " << count << ' ' << digest << std::endl;
}
