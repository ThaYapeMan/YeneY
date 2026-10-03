// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "third_party/yeney-core/sinks/shm_v1/sink.h"
#include "timing_probe_runtime.h"
#include <memory>
#include <stdexcept>
namespace timing_probe {
// Observe the existing sink's committed export coordinate; never edit its ABI.
class TimingTap {
    std::unique_ptr<yeney::ShmV1Sink> sink;
    const unsigned char *mapping = nullptr;
    int fd = -1;
    template <class T> T field(size_t offset) const {
        T v;
        std::memcpy(&v, mapping + offset, sizeof v);
        return v;
    }
    bool snapshot(uint64_t &generation, uint64_t &position) const {
        if (!mapping)
            return false;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            auto *sequence = reinterpret_cast<const uint32_t *>(mapping + 32856);
            uint32_t before = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
            if (before & 1)
                continue;
            generation = field<uint64_t>(32860);
            position = field<uint64_t>(32868);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (before == __atomic_load_n(sequence, __ATOMIC_ACQUIRE))
                return true;
        }
        return false;
    }

public:
    TimingTap(const uint8_t *mac, unsigned maximum) {
        std::array<uint8_t, 6> address;
        std::copy(mac, mac + 6, address.begin());
        const auto name = yeney::ShmV1Sink::segmentName(address);
        if (!diagnostics().configure(name.substr(std::string("/squeezelite-").size())))
            throw std::runtime_error("timing publisher already owned or unavailable");
        try {
            sink = std::make_unique<yeney::ShmV1Sink>(address, maximum);
            fd = shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
            if (fd < 0)
                throw std::runtime_error("audio tap observation open");
            auto *p = mmap(nullptr, 32888, PROT_READ, MAP_SHARED, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                fd = -1;
                throw std::runtime_error("audio tap observation mmap");
            }
            mapping = static_cast<const unsigned char *>(p);
        } catch (...) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
            diagnostics().closePublication();
            throw;
        }
    }
    ~TimingTap() {
        if (mapping)
            munmap(const_cast<unsigned char *>(mapping), 32888);
        if (fd >= 0)
            ::close(fd);
    }
    void boundary(uint64_t frame, const yeney::Format &f, bool continuous) {
        sink->trackBoundary(frame, f, continuous);
    }
    bool write(const yeney::Frame *data, size_t count, uint64_t &generation, uint64_t &first) {
        uint64_t beforeGen = 0, before = 0, afterGen = 0, after = 0;
        bool valid = snapshot(beforeGen, before);
        sink->write(data, count);
        valid = valid && snapshot(afterGen, after) && beforeGen == afterGen && after == before + count;
        generation = afterGen;
        first = before;
        return valid;
    }
    void pause() { sink->pause(); }
    void resume() { sink->resume(); }
    void flush() { sink->flush(); }
};
} // namespace timing_probe
