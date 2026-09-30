// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Host-side compatibility codec. libmad does the decoding; all queueing and
// conversion here is original. The standalone core retains minimp3 by default.
#include "compatibility_decoder.h"
#include <mad.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <stdexcept>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {
class MadDecoder : public yeney::Decoder {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<uint8_t> input;
    std::deque<yeney::Frame> output;
    bool eof = false, cancelled = false, complete = false, known = false;
    yeney::Format current;
    std::string failure;
    yeney::DecoderConfig config;
    std::thread worker;
    size_t read(uint8_t* data, size_t n) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return cancelled || eof || !input.empty(); });
        if (cancelled) return 0;
        n = std::min(n, input.size());
        for (size_t i = 0; i < n; ++i) { data[i] = input.front(); input.pop_front(); }
        return n;
    }
    bool emit(yeney::Frame frame) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return cancelled || output.size() < outputCapacity; });
        if (cancelled) return false;
        output.push_back(frame); return true;
    }
    static int32_t sample(mad_fixed_t v) {
        // Q28 -> rounded, saturated signed 24-bit -> normalised int32.
        int64_t rounded = std::max<int64_t>(-268435456, std::min<int64_t>(268435455, int64_t(v) + 16));
        int64_t pcm = rounded >= 0 ? rounded / 32 : -((-rounded + 31) / 32);
        return int32_t(pcm * 256);
    }
    void run() {
        mad_stream stream; mad_frame frame; mad_synth synth;
        mad_stream_init(&stream); mad_frame_init(&frame); mad_synth_init(&synth);
        std::vector<uint8_t> bytes; bytes.reserve(65536 + MAD_BUFFER_GUARD);
        bool end = false, first = true;
        uint64_t skip = 529, remaining = UINT64_MAX;
        try {
            for (;;) {
                if (bytes.empty() || stream.error == MAD_ERROR_BUFLEN) {
                    size_t keep = stream.next_frame ? bytes.data() + bytes.size() - stream.next_frame : bytes.size();
                    if (stream.next_frame) bytes.erase(bytes.begin(), bytes.end() - keep);
                    if (end) break;
                    uint8_t buffer[8192]; size_t n = read(buffer, sizeof(buffer));
                    bytes.insert(bytes.end(), buffer, buffer + n);
                    if (!n) { end = true; bytes.insert(bytes.end(), MAD_BUFFER_GUARD, 0); }
                    mad_stream_buffer(&stream, bytes.data(), bytes.size());
                }
                if (mad_frame_decode(&frame, &stream)) {
                    if (stream.error == MAD_ERROR_BUFLEN || MAD_RECOVERABLE(stream.error)) continue;
                    throw std::runtime_error(mad_stream_errorstr(&stream));
                }
                if (first) {
                    first = false;
                    auto begin = stream.this_frame; size_t length = stream.next_frame - begin;
                    // LAME's delay/padding is accepted only with its explicit signature,
                    // matching the installed player's handling of other encoder tags.
                    size_t xing = (frame.header.mode == MAD_MODE_SINGLE_CHANNEL ? 21 : 36);
                    if (length > xing + 8 && (!std::memcmp(begin+xing, "Xing", 4) || !std::memcmp(begin+xing, "Info", 4))) {
                        auto be = [](const uint8_t* p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; };
                        uint32_t flags = be(begin+xing+4), frames = 0; size_t at = xing+8;
                        for (auto field : {std::pair<unsigned,unsigned>{1,4}, {2,4}, {4,100}, {8,4}})
                            if (flags & field.first) {
                                if (at+field.second > length) throw std::runtime_error("truncated Xing metadata");
                                if (field.first == 1) frames = be(begin+at);
                                at += field.second;
                            }
                        if (at+24 <= length && !std::memcmp(begin+at, "LAME", 4)) {
                            unsigned delay = unsigned(begin[at+21])*16 + (begin[at+22]>>4);
                            unsigned padding = unsigned(begin[at+22]&15)*256 + begin[at+23];
                            skip = delay + 529 + 1152;
                            uint64_t trim = delay + 529 + (padding > 529 ? padding-529 : 0);
                            if (uint64_t(frames)*1152 < trim) throw std::runtime_error("invalid LAME sample range");
                            remaining = uint64_t(frames)*1152 - trim;
                        }
                    }
                }
                mad_synth_frame(&synth, &frame);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (synth.pcm.samplerate > config.maxRate) throw std::runtime_error("MP3 rate exceeds sink maximum");
                    current = {synth.pcm.samplerate, 24, synth.pcm.channels, false}; known = true;
                    if (cancelled) break;
                }
                for (unsigned i=0; i<synth.pcm.length; ++i) {
                    if (skip) { --skip; continue; }
                    if (!remaining) continue;
                    if (!emit({sample(synth.pcm.samples[0][i]), sample(synth.pcm.samples[synth.pcm.channels-1][i])})) goto done;
                    if (remaining != UINT64_MAX) --remaining;
                }
            }
        } catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mutex); failure = e.what(); }
    done:
        mad_frame_finish(&frame); mad_stream_finish(&stream);
        std::lock_guard<std::mutex> lock(mutex); complete = true;
    }
public:
    explicit MadDecoder(const yeney::DecoderConfig& c) : config(c), worker(&MadDecoder::run, this) {}
    ~MadDecoder() override { { std::lock_guard<std::mutex> lock(mutex); cancelled = true; } changed.notify_all(); worker.join(); }
    size_t feed(const uint8_t* p, size_t n) override {
        std::lock_guard<std::mutex> lock(mutex); n = std::min(n, inputCapacity-input.size());
        input.insert(input.end(), p, p+n); changed.notify_one(); return n;
    }
    void finish() override { std::lock_guard<std::mutex> lock(mutex); eof = true; changed.notify_one(); }
    size_t take(yeney::Frame* p, size_t n) override {
        std::lock_guard<std::mutex> lock(mutex); n = std::min(n, output.size());
        for(size_t i=0;i<n;++i) { p[i]=output.front(); output.pop_front(); } changed.notify_one(); return n;
    }
    bool format(yeney::Format& f) const override { std::lock_guard<std::mutex> lock(mutex); f=current; return known; }
    bool done() const override { std::lock_guard<std::mutex> lock(mutex); return complete && output.empty(); }
    std::string error() const override { std::lock_guard<std::mutex> lock(mutex); return failure; }
    size_t inputBuffered() const override { std::lock_guard<std::mutex> lock(mutex); return input.size(); }
    size_t outputBuffered() const override { std::lock_guard<std::mutex> lock(mutex); return output.size(); }
};
}
std::unique_ptr<yeney::Decoder> makeCompatibilityDecoder(const yeney::DecoderConfig& c) {
    if (c.codec == 'm') return std::make_unique<MadDecoder>(c);
    return yeney::makeDecoder(c);
}
