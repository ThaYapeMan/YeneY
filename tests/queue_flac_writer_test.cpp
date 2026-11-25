// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "flac_writer.h"
#include "packed_flac.h"
#include <FLAC/stream_decoder.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <openssl/sha.h>
#include "test_temp.h"
#include <filesystem>
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
    std::vector<uint64_t> *offsets = nullptr;

    static FLAC__StreamDecoderTellStatus tell(const FLAC__StreamDecoder *, FLAC__uint64 *position,
                                               void *opaque) {
        *position = static_cast<Decode *>(opaque)->cursor;
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder *, FLAC__byte target[],
                                               size_t *count, void *opaque) {
        auto &self = *static_cast<Decode *>(opaque);
        *count = std::min(*count, self.wire.size() - self.cursor);
        std::copy_n(self.wire.data() + self.cursor, *count, target);
        self.cursor += *count;
        return *count ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
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
        if (self.offsets) {
            FLAC__uint64 position = 0;
            assert(FLAC__stream_decoder_get_decode_position(decoder, &position));
            self.offsets->push_back(position);
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void error(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *) {
        assert(false && "reference decoder rejected a frame");
    }
};

void decode(const std::vector<char> &wire, uint64_t first, uint64_t total, unsigned bits,
            std::vector<uint64_t> *offsets = nullptr) {
    Decode state{wire, bits, 0, first, offsets};
    std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>
        decoder(FLAC__stream_decoder_new(), FLAC__stream_decoder_delete);
    assert(decoder);
    assert(FLAC__stream_decoder_init_stream(decoder.get(), Decode::read, nullptr, Decode::tell, nullptr,
        nullptr, Decode::write, nullptr, Decode::error, &state) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
    if (offsets) {
        FLAC__uint64 position = 0;
        assert(FLAC__stream_decoder_process_until_end_of_metadata(decoder.get()));
        assert(FLAC__stream_decoder_get_decode_position(decoder.get(), &position));
        offsets->push_back(position);
    }
    assert(FLAC__stream_decoder_process_until_end_of_stream(decoder.get()));
    assert(FLAC__stream_decoder_finish(decoder.get()));
    assert(state.frames == total);
}
}

namespace {
std::string sha256(const std::vector<char> &bytes) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), hash);
    std::ostringstream text;
    for (auto byte : hash)
        text << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return text.str();
}

void compareProbe(const std::vector<char> &wire, const std::string &directory,
                  unsigned rate, unsigned bits, uint64_t samples, bool packed = false) {
    const auto pcm = directory + "/audio.pcm";
    const auto out = directory + "/reference.flac";
    {
        std::ofstream stream(pcm, std::ios::binary);
        for (uint64_t frame = 0; frame < samples; ++frame)
            for (unsigned channel : {0, 1})
                for (unsigned byte = bits == 16; byte < 3; ++byte)
                    stream.put(sample(frame * 6 + channel * 3 + byte, bits));
        assert(stream.good());
    }
    const auto command = "'" + directory + "/helper' " + (packed ? "packed" : "fixed-noseek") + " '" + pcm + "' '" + out + "' " +
        std::to_string(samples) + " " + std::to_string(rate) + " " + std::to_string(bits) + " 2 256";
    assert(std::system(command.c_str()) == 0);
    std::ifstream stream(out, std::ios::binary);
    const std::vector<char> reference((std::istreambuf_iterator<char>(stream)), {});
    const auto actualHash = sha256(wire);
    const auto expectedHash = sha256(reference);
    std::cout << rate << " Hz " << bits << "-bit " << samples << " samples SHA256 bridge="
              << actualHash << " probe=" << expectedHash << std::endl;
    if (wire != reference) {
        const auto mismatch = std::mismatch(wire.begin(), wire.end(), reference.begin(), reference.end());
        std::cerr << "first differing offset=" << mismatch.first - wire.begin() << std::endl;
        assert(wire == reference);
    }
}
}

void checkLayout(unsigned bits, const std::string &directory) {
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
            compareProbe(wire, directory, rate, bits, samples);
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

void checkPacked(unsigned bits, const std::string &directory) {
    for (unsigned rate : {44100, 48000}) {
        uint64_t offset = 0;
        const uint64_t frames = 60 * rate;
        const auto packed = queue_audio::PackedFlac::encode(directory, frames, rate, bits,
            [&](char *target, size_t size) {
                for (size_t i = 0; i < size; ++i) target[i] = sample(offset++, bits);
                return size;
            }, 64 * 1024 * 1024);
        std::vector<char> wire(packed->size());
        assert(packed->read(0, wire.data(), wire.size()) == wire.size());
        compareProbe(wire, directory, rate, bits, frames, true);
        std::vector<uint64_t> offsets;
        decode(wire, 0, frames, bits, &offsets);
        assert(packed->frames() == (frames + 255) / 256);
        assert(offsets.size() == packed->frames() + 1);
        for (size_t index = 0; index < offsets.size(); ++index)
            assert(packed->offset(index) == offsets[index]);
        std::cout << "PASS packed " << rate << " Hz " << bits << "-bit bytes, PCM and frame table\n";
    }
}

int main() {
    auto directory = testTempRoot() + "/yeney-flac-writer-XXXXXX";
    assert(mkdtemp(directory.data()));
    const auto command = "g++ -std=c++17 -O2 scripts/yeney-refill-audio.cpp -lFLAC -lcrypto -o '" +
        directory + "/helper'";
    assert(std::system(command.c_str()) == 0);
    checkLayout(24, directory);
    checkLayout(16, directory);
    checkPacked(16, directory);
    checkPacked(24, directory);
    std::filesystem::remove_all(directory);
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
