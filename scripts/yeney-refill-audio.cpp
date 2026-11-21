// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/metadata.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

using Bytes = std::vector<uint8_t>;

struct Output {
    std::ofstream pcm;
    FLAC__uint64 offset = 0;
    FLAC__uint64 sample = 0;
    bool corrupt = false;
};

static FLAC__StreamDecoderWriteStatus frame(const FLAC__StreamDecoder *decoder,
                                            const FLAC__Frame *frame,
                                            const FLAC__int32 *const data[], void *context) {
    auto &out = *static_cast<Output *>(context);
    const auto &header = frame->header;
    if (header.channels < 1 || header.channels > 2 || header.bits_per_sample > 24)
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    std::vector<char> bytes(header.blocksize * 6);
    auto target = bytes.begin();
    for (unsigned i = 0; i < header.blocksize; ++i) {
        for (unsigned channel = 0; channel < 2; ++channel) {
            uint32_t value = uint32_t(data[channel % header.channels][i]) << (24 - header.bits_per_sample);
            for (unsigned byte = 0; byte < 3; ++byte)
                *target++ = char(value >> (8 * byte));
        }
    }
    out.pcm.write(bytes.data(), bytes.size());
    std::cout << out.sample << ' ' << out.offset << ' ' << header.blocksize << '\n';
    out.sample += header.blocksize;
    if (!out.pcm || !FLAC__stream_decoder_get_decode_position(decoder, &out.offset))
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void error(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *context) {
    static_cast<Output *>(context)->corrupt = true;
}

static int decode(const char *source, const char *target) {
    Output output{std::ofstream(target, std::ios::binary | std::ios::app)};
    auto decoder = std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>(
        FLAC__stream_decoder_new(), FLAC__stream_decoder_delete);
    if (!decoder || !output.pcm)
        return 1;
    auto *d = decoder.get();
    FLAC__stream_decoder_set_md5_checking(d, true);
    if (FLAC__stream_decoder_init_file(d, source, frame, nullptr, error, &output) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK)
        return 1;
    const bool decoded = FLAC__stream_decoder_process_until_end_of_metadata(d) &&
                         FLAC__stream_decoder_get_decode_position(d, &output.offset) &&
                         FLAC__stream_decoder_process_until_end_of_stream(d);
    output.pcm.close();
    const bool verified = FLAC__stream_decoder_finish(d);
    return decoded && verified && output.pcm && !output.corrupt ? 0 : 1;
}

namespace {
constexpr uint32_t blockSize = 4096;

void writeBytes(std::ostream &out, const Bytes &data) {
    out.write(reinterpret_cast<const char *>(data.data()), data.size());
}

void bigEndian(Bytes &bytes, uint64_t value, unsigned width) {
    while (width)
        bytes.push_back(value >> (8 * --width));
}

unsigned numberWidth(uint32_t number) {
    constexpr uint32_t limits[] = {1 << 7, 1 << 11, 1 << 16, 1 << 21, 1 << 26};
    return 1 + (std::upper_bound(std::begin(limits), std::end(limits), number) - std::begin(limits));
}

void frameNumber(Bytes &header, uint32_t number) {
    const auto length = numberWidth(number);
    if (length == 1) {
        header.push_back(number);
        return;
    }
    const auto lead = 0xffu << (8 - length);
    header.push_back(lead | (number >> (6 * (length - 1))));
    for (auto part = length - 1; part; --part)
        header.push_back(0x80 | ((number >> (6 * (part - 1))) & 0x3f));
}

unsigned frameSize(uint32_t index, unsigned count, unsigned width, unsigned channels) {
    return 7 + numberWidth(index) + (count == blockSize ? 0 : 2) + channels * (1 + count * width);
}

template<unsigned Bits, unsigned Polynomial>
unsigned crc(const Bytes &bytes) {
    static const auto table = [] {
        std::array<unsigned, 256> result{};
        for (unsigned i = 0; i != result.size(); ++i) {
            auto value = i << (Bits - 8);
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value << 1) ^ ((value & (1 << (Bits - 1))) ? Polynomial : 0);
            result[i] = value & ((1 << Bits) - 1);
        }
        return result;
    }();
    unsigned value = 0;
    for (auto byte : bytes)
        value = ((value << 8) & ((1 << Bits) - 1)) ^ table[byte ^ (value >> (Bits - 8))];
    return value;
}

struct PcmFile {
    std::ifstream file;
    uint64_t frames;
    unsigned rate, bits, channels;
    unsigned width() const { return bits / 8; }

    PcmFile(const char *path, uint64_t frames, unsigned rate, unsigned bits, unsigned channels)
        : file(path, std::ios::binary), frames(frames), rate(rate), bits(bits), channels(channels) {
        if (!file || !frames || frames >= (1ull << 36) ||
            !rate || rate >= (1 << 20) || (bits != 16 && bits != 24) || !channels || channels > 2)
            throw std::runtime_error("invalid PCM source description");
    }

    Bytes read(unsigned count) {
        Bytes bytes(count * channels * width());
        if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
            throw std::runtime_error("short PCM source");
        return bytes;
    }
};

void fixed(PcmFile &pcm, const char *path, bool seekTable) {
    const unsigned frames = (pcm.frames + blockSize - 1) / blockSize;
    const auto points = seekTable ? std::min(frames, 100u) : 0;
    std::vector<uint64_t> offsets{0};
    unsigned smallest = UINT32_MAX, largest = 0;
    for (unsigned i = 0; i < frames; ++i) {
        const auto count = std::min<uint64_t>(blockSize, pcm.frames - uint64_t(i) * blockSize);
        const auto size = frameSize(i, count, pcm.width(), pcm.channels);
        smallest = std::min(smallest, size);
        largest = std::max(largest, size);
        offsets.push_back(offsets.back() + size);
    }
    Bytes metadata{'f', 'L', 'a', 'C', uint8_t(seekTable ? 0 : 0x80), 0, 0, 34};
    bigEndian(metadata, blockSize, 2);
    bigEndian(metadata, blockSize, 2);
    bigEndian(metadata, smallest, 3);
    bigEndian(metadata, largest, 3);
    bigEndian(metadata, (uint64_t(pcm.rate) << 44) | (uint64_t(pcm.channels - 1) << 41) |
        (uint64_t(pcm.bits - 1) << 36) | pcm.frames, 8);
    metadata.insert(metadata.end(), 16, 0);
    if (seekTable) {
        metadata.push_back(0x83);
        bigEndian(metadata, points * 18, 3);
        for (unsigned point = 0; point < points; ++point) {
            const auto index = point * uint64_t(frames) / points;
            const auto sample = index * blockSize;
            bigEndian(metadata, sample, 8);
            bigEndian(metadata, offsets[index], 8);
            bigEndian(metadata, std::min<uint64_t>(blockSize, pcm.frames - sample), 2);
        }
    }

    std::ofstream out(path, std::ios::binary);
    writeBytes(out, metadata);
    for (unsigned i = 0; i < frames; ++i) {
        const auto count = std::min<uint64_t>(blockSize, pcm.frames - uint64_t(i) * blockSize);
        const auto samples = pcm.read(count);
        Bytes frame{0xff, 0xf8, uint8_t(count == blockSize ? 0xc0 : 0x70), uint8_t((pcm.channels - 1) << 4)};
        frameNumber(frame, i);
        if (count != blockSize)
            bigEndian(frame, count - 1, 2);
        frame.push_back(crc<8, 0x07>(frame));
        for (unsigned channel = 0; channel < pcm.channels; ++channel) {
            frame.push_back(0x02); // verbatim, zero wasted bits
            for (unsigned j = 0; j < count; ++j) {
                const auto start = (j * pcm.channels + channel) * pcm.width();
                for (unsigned byte = pcm.width(); byte; --byte)
                    frame.push_back(samples[start + byte - 1]);
            }
        }
        bigEndian(frame, crc<16, 0x8005>(frame), 2);
        if (frame.size() != offsets[i + 1] - offsets[i])
            throw std::runtime_error("fixed FLAC frame disagrees with its predicted size");
        writeBytes(out, frame);
    }
    const auto predicted = metadata.size() + offsets.back();
    if (!out || uint64_t(out.tellp()) != predicted || largest > 32768)
        throw std::runtime_error("fixed FLAC size or largest-frame proof failed");
    out.close();
    if (!out)
        throw std::runtime_error("fixed FLAC write failed");
    const auto midway = frames / 2;
    std::cout << predicted << ' ' << midway * uint64_t(blockSize) << ' '
              << metadata.size() + offsets[midway] << ' ' << largest << '\n';
}

void packed(PcmFile &pcm, const char *path) {
    const auto encoder = std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)>(
        FLAC__stream_encoder_new(), FLAC__stream_encoder_delete);
    const auto seek = std::unique_ptr<FLAC__StreamMetadata, decltype(&FLAC__metadata_object_delete)>(
        FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE), FLAC__metadata_object_delete);
    auto *e = encoder.get();
    auto *metadata = seek.get();
    const bool ready = e && metadata &&
        FLAC__stream_encoder_set_compression_level(e, 5) &&
        FLAC__stream_encoder_set_channels(e, pcm.channels) &&
        FLAC__stream_encoder_set_bits_per_sample(e, pcm.bits) &&
        FLAC__stream_encoder_set_sample_rate(e, pcm.rate) &&
        FLAC__stream_encoder_set_blocksize(e, blockSize) &&
        FLAC__stream_encoder_set_do_mid_side_stereo(e, false) &&
        FLAC__stream_encoder_set_verify(e, true) &&
        FLAC__stream_encoder_set_total_samples_estimate(e, pcm.frames) &&
        FLAC__metadata_object_seektable_template_append_spaced_points(
            metadata, std::min<uint64_t>(100, (pcm.frames + blockSize - 1) / blockSize), pcm.frames) &&
        FLAC__stream_encoder_set_metadata(e, &metadata, 1);
    if (!ready || FLAC__stream_encoder_init_file(e, path, nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        throw std::runtime_error("packed FLAC encoder setup failed");
    for (uint64_t first = 0; first < pcm.frames; first += blockSize) {
        const auto count = std::min<uint64_t>(blockSize, pcm.frames - first);
        const auto bytes = pcm.read(count);
        std::vector<FLAC__int32> values(count * pcm.channels);
        for (unsigned i = 0; i < values.size(); ++i) {
            uint32_t value = 0;
            for (unsigned byte = 0; byte < pcm.width(); ++byte)
                value |= uint32_t(bytes[i * pcm.width() + byte]) << (8 * byte);
            const int32_t sign = 1 << (pcm.bits - 1);
            values[i] = int32_t(value ^ sign) - sign;
        }
        if (!FLAC__stream_encoder_process_interleaved(e, values.data(), count))
            throw std::runtime_error("packed FLAC encode or verification failed");
    }
    if (!FLAC__stream_encoder_finish(e))
        throw std::runtime_error("packed FLAC finalize failed");
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 3)
            return decode(argv[1], argv[2]);
        if (argc != 8)
            return 2;
        const std::string mode = argv[1];
        PcmFile pcm(argv[2], std::stoull(argv[4]), std::stoul(argv[5]), std::stoul(argv[6]), std::stoul(argv[7]));
        if (mode == "packed")
            packed(pcm, argv[3]);
        else if (mode == "fixed" || mode == "fixed-noseek")
            fixed(pcm, argv[3], mode == "fixed");
        else
            return 2;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
