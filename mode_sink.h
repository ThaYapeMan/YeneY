// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "third_party/yeney-core/core/sink.h"
#include <atomic>
#include <functional>
#include <mutex>
namespace bridge {
// The room worker requests selection; the core applies it before fresh LMS audio.
// A requested generation prevents further PCM/callbacks in the old sink.
class ModeSink : public yeney::Sink {
    yeney::Sink &queue, &stream;
    mutable std::recursive_mutex gate;
    std::atomic<bool> queued{true};
    std::atomic<uint64_t> requested{1}, applied{1};
    std::atomic<bool> admitted{true}, resumeStart{false};
    yeney::Sink &sink() const { return queued ? queue : stream; }

public:
    std::function<void(uint64_t)> ready;
    ModeSink(yeney::Sink &q, yeney::Sink &s) : queue(q), stream(s) {}
    void select(bool q, uint64_t generation) {
        std::lock_guard<std::recursive_mutex> lock(gate);
        admitted = false;
        queued = q;
        requested = generation;
    }
    void bootstrap(uint64_t g) {
        requested = g;
        applied = g;
        admitted = true;
    }
    void admit() { admitted = true; }
    bool isQueue() const { return queued; }
    uint64_t playbackGeneration() const override { return requested; }
    void playbackReady(uint64_t g) override {
        if (g != requested)
            return;
        applied = g;
        if (ready)
            ready(g);
    }
    bool decodeAhead() const override { return sink().decodeAhead(); }
    bool speakerDriven() const override { return sink().speakerDriven(); }
    bool paced() const override { return sink().paced(); }
    uint32_t maxSampleRate() const override { return sink().maxSampleRate(); }
    void trackDescriptor(const yeney::TrackDescriptor &d) override {
        std::lock_guard<std::recursive_mutex> lock(gate);
        if (applied == requested)
            sink().trackDescriptor(d);
    }
    void resolvedItemBoundary(const yeney::ItemBoundary &b) override {
        std::lock_guard<std::recursive_mutex> lock(gate);
        if (applied == requested)
            sink().resolvedItemBoundary(b);
    }
    void coreEvent(const std::string &s, uint64_t a, uint64_t b) override {
        std::lock_guard<std::recursive_mutex> lock(gate);
        if (applied == requested)
            sink().coreEvent(s, a, b);
    }
    bool queueCommand(char c, uint32_t v) override { return admitted && sink().queueCommand(c, v); }
    bool takeQueueRefetch() override { return admitted && sink().takeQueueRefetch(); }
    void resumeReceivedStart() { resumeStart = true; }
    bool takeQueueStart() override {
        return admitted && applied == requested && (resumeStart.exchange(false) || sink().takeQueueStart());
    }
    bool takeQueueCachedStart() override {
        return admitted && applied == requested && sink().takeQueueCachedStart();
    }
    bool takeAudibleStart(yeney::AudibleStart &a) override { return admitted && sink().takeAudibleStart(a); }
    bool takeAudibleState(yeney::AudibleState &a) override { return admitted && sink().takeAudibleState(a); }
    std::optional<uint32_t> audibleElapsedMs(uint64_t a) const override { return sink().audibleElapsedMs(a); }
    std::optional<uint64_t> committedItemFrames(uint64_t a) const override {
        return sink().committedItemFrames(a);
    }
    bool awaitingPublication(uint64_t a) const override { return sink().awaitingPublication(a); }
    void trackBoundary(uint64_t n, const yeney::Format &f, bool b) override {
        std::lock_guard<std::recursive_mutex> lock(gate);
        if (applied == requested)
            sink().trackBoundary(n, f, b);
    }
    size_t write(const yeney::Frame *p, size_t n) override { return write(p, n, false); }
    size_t write(const yeney::Frame *p, size_t n, bool b) override {
        std::lock_guard<std::recursive_mutex> lock(gate);
        return admitted && applied == requested ? sink().write(p, n, b) : 0;
    }
    void pause() override { sink().pause(); }
    void resume() override { sink().resume(); }
    void stop() override { sink().stop(); }
    void flush() override { sink().flush(); }
    uint64_t audibleFrames() const override { return sink().audibleFrames(); }
    uint64_t startedFrames() const override { return sink().startedFrames(); }
    bool drained(uint64_t n) const override { return sink().drained(n); }
    bool outputEmpty(uint64_t n) const override { return sink().outputEmpty(n); }
    void playTiming(uint64_t a, uint64_t b, uint32_t c) override { sink().playTiming(a, b, c); }
    void volume(uint32_t a, uint32_t b) override { sink().volume(a, b); }
    void power(bool b) override { sink().power(b); }
};
} // namespace bridge
