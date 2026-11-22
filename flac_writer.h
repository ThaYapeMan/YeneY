// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace queue_audio {

class FlacLayout {
public:
    static constexpr unsigned blockSize = 256;
    static constexpr unsigned headerSize = 42;

    FlacLayout(uint64_t samples, unsigned rate);
    uint64_t frameCount() const;
    uint64_t offset(uint64_t index) const; // frameCount() denotes the end of the entity
    uint64_t size() const { return offset(frameCount()); }
    unsigned frameSamples(uint64_t index) const;
    std::array<char, headerSize> header() const;
    std::vector<char> frame(uint64_t index, const char *pcm) const;

private:
    static constexpr unsigned sampleBytes = 6;
    static constexpr unsigned frameOverhead = 10; // Excluding the encoded frame number.
    uint64_t samples;
    unsigned rate;
};

} // namespace queue_audio
