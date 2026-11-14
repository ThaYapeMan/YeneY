// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include <memory>
namespace bridge {
// All times are seconds on steady_clock; positions and RTT are milliseconds.
struct SpeakerPositionSample {
    std::string item, state;
    double positionMs = 0, sampleTime = 0, rttMs = 0;
};
class SpeakerPosition {
public:
    virtual ~SpeakerPosition() = default;
    virtual const char *name() const = 0;
    virtual const char *precision() const = 0;
    virtual std::optional<SpeakerPositionSample> position(double now) const = 0;
};
// Ordered registrations permit further implementations without changing selection.
class PositionSources {
    std::vector<std::shared_ptr<SpeakerPosition>> sources;
public:
    void add(std::shared_ptr<SpeakerPosition> source) { sources.push_back(std::move(source)); }
    void add(SpeakerPosition &source) { sources.emplace_back(&source, [](SpeakerPosition *) {}); }
    std::pair<std::shared_ptr<SpeakerPosition>, std::optional<SpeakerPositionSample>> select(double now) const {
        for (const auto &source : sources)
            if (auto sample = source->position(now)) return {source, sample};
        return {sources.empty() ? nullptr : sources.back(), {}};
    }
};
class PositionFit {
    std::deque<SpeakerPositionSample> samples;
    std::optional<SpeakerPositionSample> pending;
    static bool moving(const std::string &state) {
        return state == "PLAYING" || state == "TRANSITIONING";
    }
    double estimate(double now) const {
        std::vector<double> offsets;
        for (const auto &s : samples)
            offsets.push_back(s.positionMs + (moving(s.state) ? (now - s.sampleTime) * 1000 : 0));
        std::sort(offsets.begin(), offsets.end());
        const auto middle = offsets.size() / 2;
        return offsets.size() % 2 ? offsets[middle] : (offsets[middle - 1] + offsets[middle]) / 2;
    }
public:
    void reset() { samples.clear(); pending.reset(); }
    bool add(const SpeakerPositionSample &sample) {
        if (!std::isfinite(sample.positionMs) || !std::isfinite(sample.sampleTime) ||
            !std::isfinite(sample.rttMs) || sample.rttMs < 0 || sample.rttMs > 200 || sample.positionMs < 0)
            return false;
        if (!samples.empty() && sample.sampleTime <= samples.back().sampleTime) return false;
        if (!samples.empty() && (sample.item != samples.back().item || sample.state != samples.back().state)) reset();
        if (!samples.empty() && std::abs(sample.positionMs - estimate(sample.sampleTime)) > 150) {
            if (!pending) { pending = sample; return false; }
            const auto expected = pending->positionMs +
                (moving(sample.state) ? (sample.sampleTime - pending->sampleTime) * 1000 : 0);
            if (std::abs(sample.positionMs - expected) > 150) { pending = sample; return false; }
            const auto first = *pending;
            reset();
            samples.push_back(first);
        }
        pending.reset();
        samples.push_back(sample);
        while (samples.size() > 1 && sample.sampleTime - samples.front().sampleTime > 2.5) samples.pop_front();
        return true;
    }
    std::optional<SpeakerPositionSample> position(double now) const {
        if (samples.empty()) return {};
        auto result = samples.back();
        result.positionMs = std::max(0.0, estimate(now));
        result.sampleTime = now;
        return result;
    }
};
class UpnpSpeakerPosition : public SpeakerPosition {
    std::optional<SpeakerPositionSample> value;
public:
    const char *name() const override { return "upnp"; }
    const char *precision() const override { return "second"; }
    void update(SpeakerPositionSample sample) { value = std::move(sample); }
    std::optional<SpeakerPositionSample> position(double) const override { return value; }
};
} // namespace bridge
