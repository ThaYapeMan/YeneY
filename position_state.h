#ifndef POSITION_STATE_H
#define POSITION_STATE_H
#include <cstdint>
#include <algorithm>
#include <cmath>

// Caller holds the position mutex. Time is monotonic milliseconds; explicit
// arguments let tests cover connection/poll races without wall-clock sleeps.
class ConnectionPosition {
public:
    void reset(unsigned id) {
        stream = id; request = 0; base = relative = lastFrames = 0;
        anchored = hadAudio = heardPosition = pendingAudibleBase = false;
        audibleBase = lastAudible = 0; reportTime = 0; slewOffset = 0; reportingModel = false; ++generation;
    }
    void connection(unsigned id, uint64_t req, uint64_t now) {
        if (id != stream) reset(id);
        request = req; reportTime = 0; slewOffset = 0; reportingModel = false;
        // Hold the last reported position until the first PCM offset is known.
        base = lastFrames;
        audibleBase = lastAudible;
        relative = 0; anchored = false; pendingAudibleBase = false; started = now; ++generation;
        if (!hadAudio) base = 0;
    }
    void pcm(unsigned id, uint64_t req, uint64_t firstFrame, uint64_t now, bool deferAudible = false) {
        if (id != stream || req != request || anchored) return;
        base = hadAudio ? firstFrame : 0;
        // S7#2 (27 Sep): the first GET closed after 1.25 s, before
        // any positive RelTime. Its queued PCM is not an audible offset.
        pendingAudibleBase = deferAudible && heardPosition;
        audibleBase = heardPosition ? (deferAudible ? lastAudible : base) : 0;
        relative = 0; anchored = hadAudio = true;
        started = now; ++generation;
    }
    uint64_t measurementBase() const { return base; }
    uint64_t token() const { return generation; }
    void poll(uint64_t token, uint32_t ms, uint64_t now) {
        if (token != generation || !anchored || ms == 0) return;
        // Stop may report zero before the next GET; retain the last position.
        // A new connection/PCM anchor already clears relative to zero.
        // Position reads are cached for one second. Ignore that cache after
        // a handoff, and reject an old RelTime larger than this GET's lifetime.
        if (now - started < 1100 || ms > now - started + 1000) return;
        if (pendingAudibleBase) { audibleBase = base; pendingAudibleBase = false; }
        // Repeated leased values are the same observation, not a fresh bracket.
        if (!heardPosition || relative != ms) observed = now;
        relative = ms;
        heardPosition = true;
    }
    uint64_t frames(uint32_t rate) {
        lastFrames = base + uint64_t(relative) * rate / 1000;
        return lastFrames;
    }
    unsigned streamId() const { return stream; }
    bool modelAudibleFrames(uint32_t rate, double now, double candidate, double rateScale, uint64_t &result, double relSecond = NAN, double observedMono = NAN) {
        if (!anchored || !heardPosition || pendingAudibleBase || now < observed ||
            !std::isfinite(now) || !std::isfinite(candidate) || !std::isfinite(rateScale) || rateScale <= 0) return false;
        // RelTime is floored at observation. Project its interval to now, rather
        // than force the model onto a leased, whole-second staircase.
        // Prefer the timestamp of the actual normal SOAP read, already recorded
        // by the probe seed path, when it corresponds to this leased value.
        const double sampleMs = std::isfinite(observedMono) && relSecond == double(relative) / 1000
            ? observedMono * 1000 : double(observed);
        if (sampleMs > now) return false;
        const double elapsed = (double(now) - sampleMs) * rate * rateScale / 1000;
        const double low = double(audibleBase) + double(relative) * rate / 1000 + elapsed;
        const double high = low + rate;
        const double bounded = std::max(low, std::min(high, candidate));
        if (bounded < 0 || bounded >= double(UINT64_MAX)) return false;
        frames(rate); // preserve the encoded handoff coordinate
        lastAudible = std::max(lastAudible, uint64_t(bounded));
        result = lastAudible;
        return true;
    }
    uint64_t audibleFrames(uint32_t rate) {
        frames(rate); // retain the encoded-stream coordinate for handoffs
        const uint64_t next = audibleBase + uint64_t(relative) * rate / 1000;
        if (next > lastAudible) lastAudible = next;
        return lastAudible;
    }
    // Smooth only the audible reporting coordinate; encoded handoffs stay exact.
    uint64_t smoothFrames(uint32_t rate, double now, bool model, double candidate,
                          double scale = 1, double relSecond = NAN, double observedMono = NAN) {
        if (!anchored || !heardPosition || pendingAudibleBase || !rate) return audibleFrames(rate);
        const double sample = std::isfinite(observedMono) && relSecond == double(relative) / 1000
            ? observedMono * 1000 : double(observed);
        double target = audibleBase + (double(relative) / 1000 + .5 + std::max(0., now - sample) * scale / 1000) * rate;
        if (model) {
            const double low = target - .5 * rate;
            target = std::clamp(candidate, low, low + rate);
        }
        const double dt = reportTime ? std::max(0., now - reportTime) / 1000 : 0;
        const double projected = reportTime ? double(lastAudible) + dt * rate * scale : target;
        const double difference = target - projected;
        // 15% corrects the observed 650 ms phase error in 4.34 seconds.
        // A qualified model correction above 750 ms is a discontinuity.
        // Unlocked integer observations can skip a tick as the poll phase
        // crosses it: only explicit stream hooks may authorize their jumps.
        double next = !reportTime || (model && std::abs(difference) > .75 * rate) ? target
            : projected + std::clamp(difference, -.15 * dt * rate, .15 * dt * rate);
        lastAudible = std::max(lastAudible, uint64_t(std::max(0., next)));
        reportTime = now; reportingModel = model;
        slewOffset = (double(lastAudible) - target) / rate;
        frames(rate);
        return lastAudible;
    }
    const char *phase(bool prior) const {
        return reportingModel ? (std::abs(slewOffset) > .00005 ? "slewing" : "locked")
                              : (prior ? "prior" : "acquiring");
    }
    double offset() const { return slewOffset; }
private:
    double reportTime = 0, slewOffset = 0;
    bool reportingModel = false;
    unsigned stream = 0;
    uint64_t request = 0, generation = 0, base = 0, started = 0, lastFrames = 0, observed = 0;
    uint32_t relative = 0;
    uint64_t audibleBase = 0, lastAudible = 0;
    bool anchored = false, hadAudio = false, heardPosition = false, pendingAudibleBase = false;
};
#endif
