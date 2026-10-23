// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "core_queue_sink.h"
#include "core_sonos.h"
namespace bridge {
// The player's single sink. Selection changes only after an explicit stop or
// confirmed drain; immutable relay assets and the continuous feeder stay separate.
class QueuePathSink : public yeney::Sink {
    queue_audio::CoreQueueSink &relay;
    CoreSonosSink stream;
    std::atomic_bool relayPath{true}, ready{false}, reset{false}, waiting{false}, boundary{false};
    bool nextRelay = true;
    yeney::Command start{'s', 0, false};
    yeney::Sink &target() { return relayPath ? static_cast<yeney::Sink &>(relay) : stream; }
    const yeney::Sink &target() const { return relayPath ? static_cast<const yeney::Sink &>(relay) : stream; }

public:
    std::function<void(uint64_t)> describeSource;
    explicit QueuePathSink(queue_audio::CoreQueueSink &q, const uint8_t *mac) : relay(q), stream(mac) {}
    bool isRelay() const { return relayPath; }
    void waitBoundary() {
        boundary = false;
        waiting = true;
    }
    bool atBoundary() const { return boundary; }
    void cancelSelection() { waiting = ready = reset = false; }
    void select(bool local, bool fresh, yeney::Command c) {
        nextRelay = local;
        start = c;
        if (fresh)
            start.playing = false;
        waiting = false;
        reset = fresh;
        // Release publishes nextRelay/start/reset before the core takes ready.
        ready = true;
    }
    bool takePlaybackReset() override {
        if (!ready || !reset.exchange(false))
            return false;
        relayPath = nextRelay;
        return true;
    }
    bool takeQueueStart() override {
        if (ready.exchange(false)) {
            relayPath = nextRelay;
            if (!relayPath)
                stream.command(start);
            return true;
        }
        return relay.takeQueueStart();
    }
    void command(const yeney::Command &c) {
        if (!relayPath)
            stream.command(c);
    }
    uint32_t maxSampleRate() const override { return relay.maxSampleRate(); }
    bool decodeAhead() const override { return relayPath; }
    bool speakerDriven() const override { return relayPath; }
    bool paced() const override { return false; }
    void trackDescriptor(const yeney::TrackDescriptor &d) override { target().trackDescriptor(d); }
    void resolvedItemBoundary(const yeney::ItemBoundary &b) override { target().resolvedItemBoundary(b); }
    void coreEvent(const std::string &e, uint64_t id, uint64_t frame) override {
        if (e == "strm" && describeSource)
            describeSource(id);
        target().coreEvent(e, id, frame);
    }
    void trackBoundary(uint64_t f, const yeney::Format &v, bool gapless) override {
        target().trackBoundary(f, v, gapless);
    }
    size_t write(const yeney::Frame *p, size_t n) override { return target().write(p, n); }
    size_t write(const yeney::Frame *p, size_t n, bool dsp) override { return target().write(p, n, dsp); }
    void pause() override { target().pause(); }
    void resume() override { target().resume(); }
    void stop() override { target().stop(); }
    void flush() override { target().flush(); }
    void volume(uint32_t l, uint32_t r) override { target().volume(l, r); }
    void power(bool p) override { target().power(p); }
    uint64_t audibleFrames() const override { return target().audibleFrames(); }
    uint64_t startedFrames() const override { return target().startedFrames(); }
    bool drained(uint64_t n) const override {
        bool done = target().drained(n);
        if (waiting && done && !relayPath) {
            const_cast<QueuePathSink *>(this)->boundary = true;
            return false; // The already-received successor owns this boundary, not STMu.
        }
        return done;
    }
    bool outputEmpty(uint64_t n) const override { return target().outputEmpty(n); }
    bool queueCommand(char c, uint32_t v) override { return relayPath && relay.queueCommand(c, v); }
    bool takeQueueRefetch() override { return relayPath && relay.takeQueueRefetch(); }
    bool takeQueueCachedStart() override { return relayPath && relay.takeQueueCachedStart(); }
    bool takeQueueFailure() override { return relayPath && relay.takeQueueFailure(); }
    bool queueItemProduced(uint64_t id) const override { return target().queueItemProduced(id); }
    bool takeAudibleStart(yeney::AudibleStart &s) override { return relayPath && relay.takeAudibleStart(s); }
    bool takeAudibleState(yeney::AudibleState &s) override {
        while (relayPath && relay.takeAudibleState(s)) {
            if (waiting && s.state == yeney::PlaybackState::Stopped) {
                boundary = true;
                continue;
            }
            return true;
        }
        return false;
    }
    std::optional<uint32_t> audibleElapsedMs(uint64_t id) const override {
        return target().audibleElapsedMs(id);
    }
    std::optional<uint64_t> committedItemFrames(uint64_t id) const override {
        return target().committedItemFrames(id);
    }
    bool awaitingPublication(uint64_t id) const override { return target().awaitingPublication(id); }
};
} // namespace bridge
