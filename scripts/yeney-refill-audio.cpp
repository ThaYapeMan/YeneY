// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <FLAC/stream_decoder.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

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

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    Output output{std::ofstream(argv[2], std::ios::binary | std::ios::app)};
    auto decoder = std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>(
        FLAC__stream_decoder_new(), FLAC__stream_decoder_delete);
    if (!decoder || !output.pcm)
        return 1;
    auto *d = decoder.get();
    FLAC__stream_decoder_set_md5_checking(d, true);
    if (FLAC__stream_decoder_init_file(d, argv[1], frame, nullptr, error, &output) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK)
        return 1;
    const bool decoded = FLAC__stream_decoder_process_until_end_of_metadata(d) &&
                         FLAC__stream_decoder_get_decode_position(d, &output.offset) &&
                         FLAC__stream_decoder_process_until_end_of_stream(d);
    output.pcm.close();
    const bool verified = FLAC__stream_decoder_finish(d);
    return decoded && verified && output.pcm && !output.corrupt ? 0 : 1;
}
