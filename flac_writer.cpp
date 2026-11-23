// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "flac_writer.h"
#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace queue_audio {
namespace {
template<class Output>
void bigEndian(Output out, uint64_t value, unsigned width) {
    while (width)
        *out++ = char(value >> (8 * --width));
}

template<unsigned Bits, unsigned Polynomial>
unsigned checksum(const std::vector<char> &bytes) {
    constexpr unsigned mask = (1u << Bits) - 1;
    static constexpr auto table = [] {
        std::array<unsigned, 256> values{};
        for (unsigned i = 0; i < values.size(); ++i) {
            unsigned value = i << (Bits - 8);
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value << 1) ^ ((value & (1u << (Bits - 1))) ? Polynomial : 0);
            values[i] = value & mask;
        }
        return values;
    }();
    unsigned value = 0;
    for (unsigned char byte : bytes)
        value = ((value << 8) & mask) ^ table[byte ^ (value >> (Bits - 8))];
    return value;
}
}

unsigned FlacLayout::frameSamples(uint64_t index) const {
    if (index >= frameCount())
        throw std::out_of_range("FLAC frame index");
    return std::min<uint64_t>(blockSize, samples - index * blockSize);
}

std::array<char, FlacLayout::headerSize> FlacLayout::header() const {
    std::array<char, headerSize> bytes{'f', 'L', 'a', 'C', char(0x80), 0, 0, 34};
    const auto block = std::min<uint64_t>(samples, blockSize);
    const auto last = frameCount() - 1;
    const auto lastSize = offset(last + 1) - offset(last);
    auto smallest = lastSize;
    auto largest = lastSize;
    if (last) {
        smallest = std::min(smallest, offset(1) - offset(0));
        largest = std::max(largest, offset(last) - offset(last - 1));
    }
    bigEndian(bytes.begin() + 8, block, 2);
    bigEndian(bytes.begin() + 10, block, 2);
    bigEndian(bytes.begin() + 12, smallest, 3);
    bigEndian(bytes.begin() + 15, largest, 3);
    bigEndian(bytes.begin() + 18, (uint64_t(rate) << 44) | (1ull << 41) | (uint64_t(bits - 1) << 36) | samples, 8);
    return bytes;
}

std::vector<char> FlacLayout::frame(uint64_t index, const char *pcm) const {
    const auto count = frameSamples(index);
    std::vector<char> bytes{char(0xff), char(0xf8), char(0x60), char(bits == 16 ? 0x18 : 0x1c)};
    const auto expectedSize = offset(index + 1) - offset(index);
    bytes.reserve(expectedSize);
    unsigned width = 1 + (std::upper_bound(numberLimits.begin(), numberLimits.end(), index) - numberLimits.begin());
    if (width > 1) {
        bytes.push_back(char((0xffu << (8 - width)) | (index >> (6 * (width - 1)))));
        while (--width)
            bytes.push_back(char(0x80 | ((index >> (6 * (width - 1))) & 0x3f)));
    } else {
        bytes.push_back(char(index));
    }
    bytes.push_back(char(count - 1));
    bytes.push_back(char(checksum<8, 0x07>(bytes)));
    for (unsigned channel = 0; channel < 2; ++channel) {
        bytes.push_back(0x02);
        for (unsigned sample = 0; sample < count; ++sample) {
            const auto *littleEndian = pcm + sample * 6 + channel * 3;
            bytes.insert(bytes.end(), {littleEndian[2], littleEndian[1]});
            if (bits == 24)
                bytes.push_back(littleEndian[0]);
            else if (littleEndian[0])
                throw std::runtime_error("16-bit FLAC row contains a non-zero low byte");
        }
    }
    bigEndian(std::back_inserter(bytes), checksum<16, 0x8005>(bytes), 2);
    if (bytes.size() != expectedSize)
        throw std::logic_error("FLAC frame size disagrees with layout");
    return bytes;
}

} // namespace queue_audio
