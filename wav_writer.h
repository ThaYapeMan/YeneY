// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
namespace upnp {
class StreamRequest;
}
namespace queue_audio {
class Item;
uint64_t wavLength(uint64_t frames);
std::array<char, 44> wavHeader(uint32_t rate, uint64_t frames);
struct ByteRange {
    uint64_t first, last;
    bool partial;
};
ByteRange wavRange(const std::string &header, uint64_t length);
// Finite WAV handler shared by the production route and storage tests.
bool serveWav(upnp::StreamRequest &, const std::shared_ptr<Item> &,
              std::chrono::milliseconds wait = std::chrono::seconds(15),
              std::function<std::shared_ptr<Item>(upnp::StreamRequest &)> resolve = {});
} // namespace queue_audio
