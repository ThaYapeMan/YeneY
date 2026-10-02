#pragma once
#include <cstddef>
#include <cstdint>
// Project-owned conversion of normalized left-aligned signed stereo PCM.
// Cast to unsigned before shifting so negative values have defined behavior.
inline void packSonosSample(char* output, int32_t sample, unsigned bytes) {
    uint32_t bits = static_cast<uint32_t>(sample);
    for (unsigned i = 0; i < bytes; ++i)
        output[i] = static_cast<char>(bits >> (8 * (4 - bytes + i)));
}
