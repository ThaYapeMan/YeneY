#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Caller-owned state: deterministic seeds, no global/shared audio RNG.
class PcmDither {
    uint32_t state;

public:
    explicit PcmDither(uint32_t seed = 0x6d2b79f5u) : state(seed ? seed : 0x6d2b79f5u) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    // Difference of two independent uniform 16-bit values: triangular PDF,
    // strictly inside +/- one destination LSB in normalized 32-bit units.
    int32_t tpdf16() { return int32_t(next() >> 16) - int32_t(next() >> 16); }
};
inline int64_t pcmRound(int64_t value, unsigned shift) {
    const int64_t half = int64_t(1) << (shift - 1);
    return value >= 0 ? (value + half) / (int64_t(1) << shift) : -((-value + half) / (int64_t(1) << shift));
}
inline void packSonosSample(char *output, int32_t sample, unsigned bytes, bool processed, PcmDither &dither) {
    uint32_t bits;
    if (processed && (bytes == 2 || bytes == 3)) {
        const unsigned shift = 8 * (4 - bytes), depth = bytes * 8;
        const int64_t value = int64_t(sample) + (bytes == 2 ? dither.tpdf16() : 0);
        const int64_t limit = int64_t(1) << (depth - 1);
        bits = uint32_t(std::clamp<int64_t>(pcmRound(value, shift), -limit, limit - 1));
        for (unsigned i = 0; i < bytes; ++i)
            output[i] = char(bits >> (8 * i));
    } else {
        // Lossless normalized 16/24-bit input has zero low bits. Retain the
        // previous truncation semantics for unprocessed legacy down-packing.
        bits = uint32_t(sample);
        for (unsigned i = 0; i < bytes; ++i)
            output[i] = char(bits >> (8 * (4 - bytes + i)));
    }
}
inline void packSonosSample(char *output, int32_t sample, unsigned bytes) {
    PcmDither unused;
    packSonosSample(output, sample, bytes, false, unused);
}
