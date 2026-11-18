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
// Local API wire states are translated only here. Bare names remain supported
// for older local API implementations and the named legacy fixture variant.
inline std::string mapSpeakerState(const std::string &raw) {
    if (raw == "PLAYBACK_STATE_PLAYING" || raw == "PLAYING") return "PLAYING";
    if (raw == "PLAYBACK_STATE_PAUSED" || raw == "PAUSED") return "PAUSED";
    if (raw == "PLAYBACK_STATE_BUFFERING" || raw == "TRANSITIONING") return "TRANSITIONING";
    if (raw == "PLAYBACK_STATE_IDLE" || raw == "STOPPED") return "STOPPED";
    return "UNKNOWN";
}
inline bool speakerPlaying(const std::string &state) { return state == "PLAYING"; }
inline bool speakerTransitioning(const std::string &state) { return state == "TRANSITIONING"; }
// All times are seconds on steady_clock; positions and RTT are milliseconds.
struct SpeakerPositionSample {
    std::string item, state;
    double positionMs = 0, sampleTime = 0, rttMs = 0;
    double observedAt = 0; // midpoint for polls, arrival time for events
    bool event = false;
    std::string rawState = {};
    std::string previousItem = {};
    std::optional<double> previousPositionMs = {};
    bool held = false;
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
class PositionLink {
    std::deque<std::pair<double, double>> answers;
    size_t slow = 0;
    unsigned consecutiveSlow = 0;
    bool uneven = false;
    double changedAt = -5;

    static bool slowAnswer(double rtt) { return rtt > 50; }
public:
    bool observe(double now, double rtt) {
        while (!answers.empty() && now - answers.front().first > 30) {
            slow -= slowAnswer(answers.front().second);
            answers.pop_front();
        }
        if (answers.empty()) consecutiveSlow = 0;
        answers.emplace_back(now, rtt);
        const bool delayed = slowAnswer(rtt);
        slow += delayed;
        consecutiveSlow = delayed ? consecutiveSlow + 1 : 0;
        if (answers.size() < 12 || now - changedAt < 5)
            return false;
        if (!uneven && (consecutiveSlow >= 3 || (slow >= 3 && slow * 10 > answers.size()))) {
            uneven = true;
        } else if (uneven && now - answers.front().first >= 25 && slow * 50 <= answers.size()) {
            uneven = false;
        } else {
            return false;
        }
        changedAt = now;
        return true;
    }
    bool isUneven() const { return uneven; }
    double quickLimit() const {
        if (answers.empty()) return 50;
        const auto quickest = std::min_element(answers.begin(), answers.end(), [](const auto &left, const auto &right) {
            return left.second < right.second;
        });
        return std::min(50.0, quickest->second + 12);
    }
    double trustedLimit() const { return uneven ? quickLimit() : 200; }
};

class PositionFit {
    std::deque<SpeakerPositionSample> samples;
    std::optional<SpeakerPositionSample> pending, lastQuick;
    static bool moving(const std::string &state) {
        return speakerPlaying(state);
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
    void reset() { samples.clear(); pending.reset(); lastQuick.reset(); }
    void retainQuickAnswers(double limit) {
        const auto quick = [limit](const auto &sample) { return sample.event || sample.rttMs <= limit; };
        if (std::any_of(samples.begin(), samples.end(), quick))
            samples.erase(std::remove_if(samples.begin(), samples.end(), [&](const auto &sample) {
                return !quick(sample);
            }), samples.end());
        else if (lastQuick && quick(*lastQuick))
            samples = {*lastQuick};
        if (pending && !quick(*pending)) pending.reset();
    }
    bool add(const SpeakerPositionSample &sample, double quickLimit = 50) {
        if (!std::isfinite(sample.positionMs) || !std::isfinite(sample.sampleTime) ||
            !std::isfinite(sample.rttMs) || sample.rttMs < 0 || sample.rttMs > 200 || sample.positionMs < 0)
            return false;
        if (sample.event) reset();
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
        if (sample.event || sample.rttMs <= quickLimit) lastQuick = sample;
        while (samples.size() > 1 && sample.sampleTime - samples.front().sampleTime > 2.5) samples.pop_front();
        return true;
    }
    std::optional<SpeakerPositionSample> position(double now) const {
        if (samples.empty()) return {};
        auto result = samples.back();
        result.positionMs = std::max(0.0, estimate(now));
        result.observedAt = samples.back().sampleTime;
        result.sampleTime = now;
        return result;
    }
    std::optional<double> precisionMs() const {
        if (samples.empty() || samples.back().event) return {};
        const auto slowest = std::max_element(samples.begin(), samples.end(), [](const auto &left, const auto &right) {
            return left.rttMs < right.rttMs;
        });
        return slowest->rttMs / 2;
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
