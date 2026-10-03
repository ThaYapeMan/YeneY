// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
namespace timing_probe {
struct alignas(8) TimingRecord {
    uint32_t magic = 0x4d544e59;
    uint16_t version = 1, flags = 1;
    uint32_t write_seq = 0, size = 128;
    uint64_t shm_generation = 0, model_epoch = 0;
    uint32_t state = 0, discontinuity = 0;
    uint64_t anchor_abs_frame = 0, anchor_audible_mono_ns = 0;
    uint32_t sample_rate_hz = 0;
    int32_t drift_ppb = 0;
    uint32_t uncertainty_us = UINT32_MAX;
    int32_t offset_us = 0;
    uint64_t updated_mono_ns = 0, valid_from_abs_frame = 0, valid_until_abs_frame = 0;
    uint64_t reserved[4]{};
};
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "timing channel is little endian");
static_assert(sizeof(TimingRecord) == 128);
static_assert(offsetof(TimingRecord, write_seq) == 8 && offsetof(TimingRecord, shm_generation) == 16);
static_assert(offsetof(TimingRecord, state) == 32 && offsetof(TimingRecord, anchor_abs_frame) == 40);
static_assert(offsetof(TimingRecord, sample_rate_hz) == 56 && offsetof(TimingRecord, updated_mono_ns) == 72);
static_assert(offsetof(TimingRecord, version) == 4 && offsetof(TimingRecord, flags) == 6 &&
              offsetof(TimingRecord, size) == 12);
static_assert(offsetof(TimingRecord, model_epoch) == 24 && offsetof(TimingRecord, discontinuity) == 36 &&
              offsetof(TimingRecord, anchor_audible_mono_ns) == 48);
static_assert(offsetof(TimingRecord, drift_ppb) == 60 && offsetof(TimingRecord, uncertainty_us) == 64 &&
              offsetof(TimingRecord, offset_us) == 68);
static_assert(offsetof(TimingRecord, valid_from_abs_frame) == 80 &&
              offsetof(TimingRecord, valid_until_abs_frame) == 88 && offsetof(TimingRecord, reserved) == 96);
// Atomic 32-bit words avoid C++ data races as well as hardware torn reads.
inline void storeRecord(TimingRecord *dest, const TimingRecord &source) {
    auto *out = reinterpret_cast<uint32_t *>(dest);
    uint32_t words[32];
    std::memcpy(words, &source, 128);
    uint32_t seq = __atomic_load_n(out + 2, __ATOMIC_SEQ_CST);
    seq = (seq & 1) ? seq + 2 : seq + 1;
    __atomic_store_n(out + 2, seq, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < 32; ++i)
        if (i != 2)
            __atomic_store_n(out + i, words[i], __ATOMIC_SEQ_CST);
    __atomic_store_n(out + 2, seq + 1, __ATOMIC_SEQ_CST);
}
inline bool loadRecord(const TimingRecord *src, TimingRecord &result) {
    auto *in = reinterpret_cast<const uint32_t *>(src);
    uint32_t seq = __atomic_load_n(in + 2, __ATOMIC_SEQ_CST), words[32];
    if (seq & 1)
        return false;
    for (unsigned i = 0; i < 32; ++i)
        words[i] = __atomic_load_n(in + i, __ATOMIC_SEQ_CST);
    if (seq != __atomic_load_n(in + 2, __ATOMIC_SEQ_CST))
        return false;
    std::memcpy(&result, words, 128);
    return true;
}
class Publisher {
    std::string name;
    int fd = -1;
    TimingRecord *mapping = nullptr;

public:
    ~Publisher() { close(); }
    bool open(const std::string &mac) {
        close();
        name = "/yeney-timing-" + mac;
        fd = shm_open(name.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0)
            return false;
        if (flock(fd, LOCK_EX | LOCK_NB) || ftruncate(fd, 128)) {
            ::close(fd);
            fd = -1;
            return false;
        }
        void *p = mmap(nullptr, 128, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            ::close(fd);
            fd = -1;
            return false;
        }
        mapping = static_cast<TimingRecord *>(p);
        storeRecord(mapping, TimingRecord{});
        return true;
    }
    bool active() const { return mapping; }
    void write(const TimingRecord &r) {
        if (mapping)
            storeRecord(mapping, r);
    }
    void close() {
        if (mapping) {
            storeRecord(mapping, TimingRecord{});
            munmap(mapping, 128);
            mapping = nullptr;
            shm_unlink(name.c_str());
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};
struct FrameMapping {
    unsigned stream = 0, rate = 0;
    uint64_t generation = 0, stream_first = 0, abs_first = 0, frames = 0, valid_from = 0;
    bool valid = false;
    // A single affine run. Reader validity excludes older ring epochs/runs.
    bool add(unsigned s, unsigned hz, uint64_t gen, uint64_t pcm, uint64_t absolute, uint64_t count) {
        bool discontinuity = !valid || stream != s || rate != hz || generation != gen ||
                             pcm != stream_first + frames || absolute != abs_first + frames;
        if (discontinuity) {
            stream = s;
            rate = hz;
            generation = gen;
            stream_first = pcm;
            abs_first = absolute;
            frames = 0;
            valid_from = absolute;
            valid = true;
        }
        frames += count;
        return discontinuity;
    }
    bool absolute(uint64_t frame, uint64_t &out) const {
        if (!valid || frame < stream_first)
            return false;
        out = abs_first + (frame - stream_first);
        return true;
    }
};
} // namespace timing_probe
