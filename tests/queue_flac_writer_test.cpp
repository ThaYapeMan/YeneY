// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "flac_writer.h"
#include <FLAC/stream_decoder.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

using queue_audio::FlacLayout;

namespace {
unsigned char sample(uint64_t byte, unsigned bits) {
    if (bits == 16 && byte % 3 == 0) return 0;
    return (byte ^ (byte >> 8) ^ (byte >> 16)) & 255;
}

struct Decode {
    const std::vector<char> &wire;
    unsigned bits;
    size_t cursor = 0;
    uint64_t frames = 0;

    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder *, FLAC__byte target[],
                                               size_t *count, void *opaque) {
        auto &self = *static_cast<Decode *>(opaque);
        *count = std::min(*count, self.wire.size() - self.cursor);
        std::copy_n(self.wire.data() + self.cursor, *count, target);
        self.cursor += *count;
        return *count ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder *, const FLAC__Frame *frame,
                                                const FLAC__int32 *const pcm[], void *opaque) {
        auto &self = *static_cast<Decode *>(opaque);
        assert(frame->header.bits_per_sample == self.bits && frame->header.channels == 2);
        for (unsigned i = 0; i < frame->header.blocksize; ++i)
            for (unsigned channel = 0; channel < 2; ++channel)
                for (unsigned byte = 0; byte < 3; ++byte) {
                    const auto expected = sample((self.frames + i) * 6 + channel * 3 + byte, self.bits);
                    assert(((uint32_t(pcm[channel][i]) << (24 - self.bits)) >> (8 * byte) & 255) == expected);
                }
        self.frames += frame->header.blocksize;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void error(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *) {
        assert(false && "reference decoder rejected a frame");
    }
};

void decode(const std::vector<char> &wire, uint64_t first, uint64_t total, unsigned bits) {
    Decode state{wire, bits, 0, first};
    std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>
        decoder(FLAC__stream_decoder_new(), FLAC__stream_decoder_delete);
    assert(decoder);
    assert(FLAC__stream_decoder_init_stream(decoder.get(), Decode::read, nullptr, nullptr, nullptr,
        nullptr, Decode::write, nullptr, Decode::error, &state) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
    assert(FLAC__stream_decoder_process_until_end_of_stream(decoder.get()));
    assert(FLAC__stream_decoder_finish(decoder.get()));
    assert(state.frames == total);
}
}

void checkLayout(unsigned bits) {
    for (const unsigned rate : {44100, 48000}) {
        for (const uint64_t samples : {1, 255, 256, 257, 1000000, 16811890}) {
            const FlacLayout layout(samples, rate, bits);
            const auto info = layout.header();
            std::vector<char> wire(info.begin(), info.end());
            wire.reserve(layout.size());
            for (uint64_t index = 0; index < layout.frameCount(); ++index) {
                if (index % 100 == 0)
                    assert(layout.offset(index) == wire.size());
                std::array<char, FlacLayout::blockSize * 6> pcm{};
                for (size_t byte = 0; byte < pcm.size(); ++byte)
                    pcm[byte] = sample(index * pcm.size() + byte, bits);
                const auto frame = layout.frame(index, pcm.data());
                wire.insert(wire.end(), frame.begin(), frame.end());
            }
            assert(wire.size() == layout.size());
            decode(wire, 0, samples, bits);
            for (const auto index : {uint64_t(0), layout.frameCount() / 2, layout.frameCount() - 1}) {
                std::vector<char> suffix(info.begin(), info.end());
                suffix.insert(suffix.end(), wire.begin() + layout.offset(index), wire.end());
                decode(suffix, index * FlacLayout::blockSize, samples, bits);
            }
            std::cout << "PASS " << rate << " Hz " << bits << "-bit " << samples << " samples: size, offsets and libFLAC PCM\n";
        }
    }
}

int main() {
    checkLayout(24);
    checkLayout(16);
    const FlacLayout layout(1, 44100, 16);
    const std::array<char, 6> notSixteenBit{1, 0, 0, 0, 0, 0};
    bool rejected = false;
    try { layout.frame(0, notSixteenBit.data()); }
    catch (const std::runtime_error &error) {
        rejected = std::string(error.what()).find("non-zero low byte") != std::string::npos;
    }
    assert(rejected);
    std::cout << "PASS 16-bit writer rejects a non-zero low byte\n";
}
