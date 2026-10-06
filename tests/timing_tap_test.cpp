// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "timing_tap.h"
#include <cassert>
#include <sys/stat.h>
int main() {
    uint8_t mac[] = {2, 0, 0, 2, uint8_t(getpid() >> 8), uint8_t(getpid())};
    std::array<uint8_t, 6> address;
    std::copy(mac, mac + 6, address.begin());
    auto name = yeney::ShmV1Sink::segmentName(address);
    {
        timing_probe::TimingTap tap(mac, 48000);
        yeney::Format f;
        f.rate = 44100;
        tap.boundary(100, f, false);
        std::vector<yeney::Frame> pcm(256);
        pcm[0] = {0x10000, -0x10000};
        uint64_t gen, first;
        assert(tap.write(pcm.data(), pcm.size(), gen, first) && gen && first == 0);
        int fd = shm_open(name.c_str(), O_RDONLY, 0);
        assert(fd >= 0);
        struct stat status{};
        assert(fstat(fd, &status) == 0 && status.st_size == off_t(sizeof(yeney::shm_v1::TimedLayout)));
        yeney::shm_v1::TimingBlock optional{};
        assert(pread(fd, &optional, sizeof optional, sizeof(yeney::shm_v1::Layout)) == ssize_t(sizeof optional));
        // A Sonos analysis export is not a paced playback schedule.
        for (auto byte : optional.anchor_play_mono_ns) assert(byte == 0);
        for (auto byte : optional.rate_milli_hz) assert(byte == 0);
        auto *wire =
            static_cast<const yeney::shm_v1::Layout *>(mmap(nullptr, 32888, PROT_READ, MAP_SHARED, fd, 0));
        assert(wire != MAP_FAILED && !(wire->extension.flags & 1) && wire->rate == 44100 && wire->pcm[0] == 1 && wire->pcm[1] == -1);
        bool refused = false;
        try {
            timing_probe::TimingTap duplicate(mac, 48000);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
        tap.pause();
        tap.flush();
        tap.resume();
        uint64_t gen2;
        assert(tap.write(pcm.data(), pcm.size(), gen2, first) && first == 256 && gen2 == gen);
        f.rate = 48000;
        tap.boundary(0, f, false);
        assert(tap.write(pcm.data(), pcm.size(), gen2, first) && first == 512 && gen2 == gen &&
               wire->rate == 48000);
        assert(!(wire->extension.flags & 1));
        munmap(const_cast<yeney::shm_v1::Layout *>(wire), 32888);
        close(fd);
    }
    timing_probe::diagnostics().reset("shutdown", true);
    shm_unlink(name.c_str());
    puts("PASS: real core SHM sink exact accepted frame counts, existing PCM quantization/ABI, pause/flush "
         "absolute continuity, rate change and unpaced play-timing left unset");
}
