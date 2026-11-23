// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace queue_audio {

class FlacLayout {
public:
    static constexpr unsigned blockSize = 256;
    static constexpr unsigned headerSize = 42;

    FlacLayout(uint64_t samples, unsigned rate, unsigned bits = 24)
        : samples(samples), rate(rate), bits(bits) {
        if (!samples || samples >= (1ull << 36) || !rate || rate >= (1u << 20) || (bits != 16 && bits != 24))
            throw std::invalid_argument("invalid FLAC row description");
    }
    uint64_t totalSamples() const { return samples; }
    uint64_t frameCount() const { return (samples + blockSize - 1) / blockSize; }
    // frameCount() denotes the end of the entity.
    uint64_t offset(uint64_t index) const {
        if (index > frameCount())
            throw std::out_of_range("FLAC frame offset");
        auto total = headerSize + std::min(samples, index * blockSize) * (bits / 4) +
            index * (frameOverhead + 1);
        for (auto limit : numberLimits) {
            if (index <= limit)
                break;
            total += index - limit;
        }
        return total;
    }
    uint64_t size() const { return offset(frameCount()); }
    unsigned frameSamples(uint64_t index) const;
    std::array<char, headerSize> header() const;
    std::vector<char> frame(uint64_t index, const char *pcm) const;

private:
    static constexpr std::array<uint64_t, 5> numberLimits{1 << 7, 1 << 11, 1 << 16, 1 << 21, 1 << 26};
    static constexpr unsigned frameOverhead = 10; // Excluding the encoded frame number.
    uint64_t samples;
    unsigned rate, bits;
};

} // namespace queue_audio
