// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "packed_flac.h"
#include "flac_writer.h"
#include <FLAC/stream_encoder.h>
#include <FLAC/metadata.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace queue_audio {
namespace {
struct Sink {
    FILE *file;
    uint64_t limit;
    std::vector<uint64_t> &offsets;

    static FLAC__StreamEncoderWriteStatus write(const FLAC__StreamEncoder *, const FLAC__byte *p,
                                                size_t n, unsigned samples, unsigned, void *opaque) {
        auto &self = *static_cast<Sink *>(opaque);
        const auto at = ftello(self.file);
        if (at < 0 || uint64_t(at) > self.limit || n > self.limit - at)
            return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
        if (samples) {
            try { self.offsets.push_back(at); }
            catch (...) { return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR; }
        }
        return fwrite(p, 1, n, self.file) == n ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK :
                                               FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
    static FLAC__StreamEncoderSeekStatus seek(const FLAC__StreamEncoder *, FLAC__uint64 at, void *opaque) {
        auto &self = *static_cast<Sink *>(opaque);
        return at <= self.limit && !fseeko(self.file, at, SEEK_SET) ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK :
                                                                    FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    static FLAC__StreamEncoderTellStatus tell(const FLAC__StreamEncoder *, FLAC__uint64 *at, void *opaque) {
        const auto where = ftello(static_cast<Sink *>(opaque)->file);
        if (where < 0) return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
        *at = where;
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }
};
}

std::shared_ptr<const PackedFlac> PackedFlac::encode(const std::filesystem::path &directory,
                uint64_t frames, unsigned rate, unsigned bits, const ReadPcm &readPcm, uint64_t limit) {
    const FlacLayout layout(frames, rate, bits);
    const auto fd = open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "packed row file");
    FILE *file = fdopen(fd, "w+b");
    if (!file) {
        close(fd);
        throw std::runtime_error("packed row file stream");
    }
    std::unique_ptr<FILE, CloseFile> guard(file);
    std::shared_ptr<PackedFlac> packed(new PackedFlac(guard.release()));
    Sink sink{file, limit, packed->offsets};
    std::unique_ptr<FLAC__StreamMetadata, decltype(&FLAC__metadata_object_delete)>
        seek(FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE), FLAC__metadata_object_delete);
    std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)>
        encoder(FLAC__stream_encoder_new(), FLAC__stream_encoder_delete);
    auto *e = encoder.get();
    auto *metadata = seek.get();
    constexpr unsigned block = FlacLayout::blockSize;
    const bool ready = e && metadata &&
        FLAC__stream_encoder_set_compression_level(e, 5) &&
        FLAC__stream_encoder_set_channels(e, 2) &&
        FLAC__stream_encoder_set_bits_per_sample(e, bits) &&
        FLAC__stream_encoder_set_sample_rate(e, rate) &&
        FLAC__stream_encoder_set_blocksize(e, block) &&
        FLAC__stream_encoder_set_do_mid_side_stereo(e, false) &&
        FLAC__stream_encoder_set_verify(e, true) &&
        FLAC__stream_encoder_set_total_samples_estimate(e, frames) &&
        FLAC__metadata_object_seektable_template_append_spaced_points(metadata,
            std::min<uint64_t>(100, layout.frameCount()), frames) &&
        FLAC__stream_encoder_set_metadata(e, &metadata, 1);
    if (!ready || FLAC__stream_encoder_init_stream(e, Sink::write, Sink::seek, Sink::tell,
        nullptr, &sink) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        throw std::runtime_error("packed FLAC encoder setup failed");
    std::array<char, block * 6> pcm;
    std::array<FLAC__int32, block * 2> values;
    for (uint64_t first = 0; first < frames; first += block) {
        const auto count = std::min<uint64_t>(block, frames - first);
        for (size_t done = 0; done < count * 6;) {
            const auto n = readPcm(pcm.data() + done, count * 6 - done);
            if (!n) throw std::runtime_error("short PCM while packing");
            done += n;
        }
        for (size_t i = 0; i < count * 2; ++i) {
            const auto *p = reinterpret_cast<const unsigned char *>(pcm.data() + 3 * i);
            if (bits == 16 && p[0]) throw std::runtime_error("16-bit packed FLAC contains a non-zero low byte");
            uint32_t value = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
            auto sample = int32_t(value ^ 0x800000u) - 0x800000;
            values[i] = bits == 16 ? sample / 256 : sample;
        }
        if (!FLAC__stream_encoder_process_interleaved(e, values.data(), count))
            throw std::runtime_error("packed FLAC encode or verification failed");
    }
    if (!FLAC__stream_encoder_finish(e) || fflush(file) || fseeko(file, 0, SEEK_END) ||
        packed->offsets.size() != layout.frameCount())
        throw std::runtime_error("packed FLAC finalize failed");
    const auto length = ftello(file);
    if (length <= 0) throw std::runtime_error("packed FLAC size unavailable");
    packed->bytes = length;
    packed->offsets.push_back(length);
    return packed;
}

size_t PackedFlac::read(uint64_t offset, char *target, size_t count) const {
    if (offset >= size()) return 0;
    ssize_t received;
    do { received = pread(fileno(file.get()), target, std::min<uint64_t>(count, size() - offset), offset); }
    while (received < 0 && errno == EINTR);
    if (received < 0) throw std::system_error(errno, std::generic_category(), "packed FLAC read");
    return received;
}
}
