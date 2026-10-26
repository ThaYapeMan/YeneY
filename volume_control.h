// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
namespace bridge {
struct VolumeSettings {
    bool speaker = false;
    int maximum = 100;
    std::string source = "default";
};
VolumeSettings volumeSettings(const std::string &room, const char *option, const char *path);
// One serialized controller owns both directions. Values are LMS integer mixer
// percentages, never SlimProto gains and never applied to PCM.
class VolumeSync {
    struct Field { std::optional<int> lms, speaker, pending, attempted, expected; uint64_t changed = 0, confirmUntil = 0; };
    Field volume, mute;
    uint64_t lastWrite = 0;
    int maximum;
    bool capped = false;
public:
    explicit VolumeSync(int maximum) : maximum(maximum) {}
    void reset() { volume = {}; mute = {}; }
    void observeLms(int v, int m, uint64_t now) {
        auto observe = [&](Field &f, int value) {
            if (f.lms == value) return;
            f.lms = value; f.pending = value; f.attempted.reset(); f.changed = now;
        };
        observe(volume, v); observe(mute, m);
    }
    void observeSpeaker(int v, int m, uint64_t now, const std::function<void(const std::string &)> &send) {
        auto observe = [&](Field &f, int value, const char *command) {
            bool confirmationExpired = false;
            if (f.expected) {
                const bool confirmed = value == *f.expected;
                if (confirmed || now < f.confirmUntil) {
                    // Group changes can arrive in intermediate steps. Confirm
                    // our target before classifying later changes as app input.
                    f.speaker = value;
                    if (confirmed) {
                        if (f.pending && (&f == &volume ? std::min(*f.pending, maximum) : *f.pending) == value)
                            f.pending.reset();
                        f.expected.reset(); f.attempted.reset();
                    }
                    return;
                }
                f.expected.reset(); confirmationExpired = true;
            }
            if (confirmationExpired || (f.speaker && f.speaker != value)) {
                // A change before our write is an app action. An uncertain
                // write reaching its target is confirmation, not an app echo.
                const bool confirmation = f.attempted == value;
                f.pending.reset(); f.attempted.reset();
                if (!confirmation && f.lms != value) {
                    f.lms = value;
                    send(std::string(command) + std::to_string(value));
                }
            }
            f.speaker = value;
        };
        observe(volume, v, "mixer volume "); observe(mute, m, "mixer muting ");
    }
    void write(uint64_t now, const std::function<bool(bool, int)> &send,
               const std::function<void()> &capLog, const std::function<bool()> &prepare = {}) {
        auto apply = [&](Field &f, bool muting) {
            if (!f.pending || !f.speaker || now < f.changed + 200) return;
            const int desired = muting ? *f.pending : std::min(*f.pending, maximum);
            if (desired != *f.pending && !capped) { capped = true; capLog(); }
            if (f.speaker == desired) {
                if (f.expected && f.expected != desired) return;
                f.pending.reset(); f.attempted.reset(); return;
            }
            if (lastWrite && now < lastWrite + 200) return;
            lastWrite = now;
            if (!muting && prepare && prepare()) return;
            // An uncertain write is verified by the next authoritative read.
            f.attempted = desired; f.expected = desired; f.confirmUntil = now + 2000;
            if (send(muting, desired)) { f.speaker = desired; f.pending.reset(); f.attempted.reset(); }
        };
        apply(volume, false); apply(mute, true);
    }
};
// Immutable startup settings, also used by the existing room-status publisher.
inline VolumeSettings &roomVolumeSettings() { static VolumeSettings value; return value; }
} // namespace bridge
