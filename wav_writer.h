// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include "queue_format.h"
#include "flac_writer.h"
namespace upnp {
class StreamRequest;
}
namespace queue_audio {
class Item;
class PackedFlac;
inline uint64_t wavLength(uint64_t frames) {
    if (frames > (uint64_t(UINT32_MAX) - 36) / 6)
        throw std::invalid_argument("WAV exceeds RIFF limit");
    return 44 + frames * 6;
}
inline uint64_t rowLength(Format format, uint64_t frames, unsigned rate, unsigned bits = 24) {
    return format == Format::Flac ? FlacLayout(frames, rate, bits).size() : wavLength(frames);
}
std::array<char, 44> wavHeader(uint32_t rate, uint64_t frames);
struct ByteRange {
    uint64_t first, last;
    bool partial;
};
ByteRange wavRange(const std::string &header, uint64_t length);
struct Pacing {
    bool enabled = true;
    double rate = 2.0;
    unsigned freeMs = 2000;
    unsigned tailMs = 0;

    static Pacing environment();
    void log() const;
};
bool serveRow(upnp::StreamRequest &, const std::shared_ptr<Item> &, Format,
              std::chrono::milliseconds wait,
              std::function<std::shared_ptr<Item>(upnp::StreamRequest &)> resolve, bool audioPinned,
              const Pacing &pacing, unsigned bits = 24, std::shared_ptr<const PackedFlac> packed = {});

// WAV entry point for callers that explicitly serve WAV.
bool serveWav(upnp::StreamRequest &, const std::shared_ptr<Item> &,
              std::chrono::milliseconds wait = std::chrono::seconds(15),
              std::function<std::shared_ptr<Item>(upnp::StreamRequest &)> resolve = {}, bool audioPinned = false,
              const Pacing &pacing = {});
} // namespace queue_audio
