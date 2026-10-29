// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "audio_mode.h"
#include "queue_item_store.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace queue_audio {
struct QueueRecord {
    yeney::TrackDescriptor source;
    std::chrono::steady_clock::time_point strmAt{};
    uint64_t expectedIndex = 0;
    uint64_t parent = 0, segmentOffset = 0;
    bool estimated = false, productionComplete = false;
    double duration = 0;
    uint32_t sourceOriginMs = 0;
    double strm = -1, decoded = -1, stmd = -1, audible = -1, stms = -1, end = -1;
    uint64_t first = 0, actual = 0, overlap = 0, prefixFrames = 0;
    std::optional<uint64_t> promised, finalFrame;
    std::string publication = "pending", transition, fault, incomplete;
    bool storageFault = false;
    struct LmsSample {
        double stamp;
        std::string index, time, title;
        std::optional<bool> matches;
        double audibleElapsed = -1, timeErrorWall = -1;
    };
    std::vector<LmsSample> lms;
    double mismatchedSeconds = 0, mismatchRun = 0;
    bool lmsMismatch = false;
    std::shared_ptr<Item> item;
};
class CoreQueueSink : public yeney::Sink {
    struct Job {
        double seconds;
        std::function<void()> run;
    };
    Store store;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    std::thread worker;
    bool closing = false, busy = false, paused = false;
    double staged = 0, budget, mismatchSince = -1, clockSpeed;
    std::map<uint64_t, QueueRecord> records;
    std::deque<uint64_t> descriptorOrder;
    std::deque<yeney::AudibleStart> audibleSignals;
    std::deque<yeney::AudibleState> speakerStates;
    bool speakerClock = false;
    std::atomic_bool queueFailed{false};
    std::function<bool(char, uint32_t)> commandHandler;
    std::optional<yeney::PlaybackState> speakerState;
    std::optional<uint32_t> speakerElapsed;
    uint64_t describedLast = 0, active = 0, audibleId = 0, audibleFrame = 0, submitted = 0;
    uint64_t continuationSerial = UINT64_MAX / 2, producingSegment = 0;
    uint64_t lastEnded = 0, nextIndex = 0, audibleSource = 0;
    bool prefixPending = false, freshRender = false, cachedReanchor = false;
    std::atomic_bool refetchRequested{false}, startRequested{false}, cachedStartRequested{false};
    std::atomic<uint64_t> latestProducer{0};
    uint32_t rate = 48000;
    std::string workerFault;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    double now() const;
    void enqueue(std::function<void()>, double seconds = 0);
    void work();
    void append(const std::vector<yeney::Frame> &, bool);
    void appendEstimated(const std::vector<yeney::Frame> &, bool);
    void protectItems();
    void commit(uint64_t previous, const yeney::TrackDescriptor &next);
    void finish(const yeney::ItemBoundary &);

public:
    explicit CoreQueueSink(const std::string &room, Config config, double seconds = 30, double speed = 1);
    ~CoreQueueSink() override;
    bool decodeAhead() const override { return true; }
    uint32_t maxSampleRate() const override { return audioMode() == AudioMode::Legacy ? 44100 : 48000; }
    bool paced() const override { return false; }
    void estimate(uint64_t occurrence, double duration, unsigned originMs);
    void trackDescriptor(const yeney::TrackDescriptor &) override;
    void coreEvent(const std::string &, uint64_t, uint64_t) override;
    void resolvedItemBoundary(const yeney::ItemBoundary &) override;
    void trackBoundary(uint64_t, const yeney::Format &, bool) override;
    size_t write(const yeney::Frame *p, size_t n) override { return write(p, n, false); }
    size_t write(const yeney::Frame *, size_t, bool) override;
    // Explicit audible control, used only by a device controller or the dry-run
    // clock.
    void audibleFrom(uint64_t occurrence, uint64_t firstFrame);
    bool isPaused() const;
    // Enable before starting the player. Only the controller calls this path,
    // after speaker identity/position confirmation; never an HTTP reader.
    void useSpeakerClock();
    // Bind before Player::run. The handler enqueues work and must not wait.
    void onQueueCommand(std::function<bool(char, uint32_t)>);
    bool queueCommand(char, uint32_t) override;
    // Before an LMS re-render/seek run: do not carry an old outgoing tail
    // into the independently selected asset. Cached mixed intros are untouched.
    void beginRender();
    void restartCached();
    uint64_t latestOccurrence() const { return latestProducer.load(); }
    void resumeReceivedStart() { startRequested = true; }
    bool takeQueueStart() override { return startRequested.exchange(false); }
    void confirmCachedStart() {
        refetchRequested = true;
        cachedStartRequested = true;
    }
    bool takeQueueCachedStart() override { return cachedStartRequested.exchange(false); }
    bool takeQueueRefetch() override { return refetchRequested.exchange(false); }
    void reanchorSpeaker(uint64_t occurrence, uint32_t sourceOriginMs);
    void speakerPosition(uint64_t occurrence, uint64_t renderedFrame, yeney::PlaybackState);
    bool speakerDriven() const override;
    std::optional<yeney::EncodedSpool> encodedSpool(uint64_t) override;
    bool queueEndRequiresPlayed(uint64_t) const override;
    bool queueItemProduced(uint64_t) const override;
    bool queuePrefixProduced(uint64_t, uint64_t) const override;
    void failQueueTrack() { queueFailed = true; }
    bool takeQueueFailure() override { return queueFailed.exchange(false); }
    bool takeAudibleState(yeney::AudibleState &) override;
    std::optional<uint32_t> audibleElapsedMs(uint64_t occurrence) const override;
    void audibleEnd(uint64_t occurrence);
    void recordLms(uint64_t occurrence, QueueRecord::LmsSample sample);
    void advanceAudible(uint64_t cumulativeFrame);
    bool takeAudibleStart(yeney::AudibleStart &) override;
    std::optional<uint64_t> committedItemFrames(uint64_t) const override;
    bool awaitingPublication(uint64_t) const override;
    uint64_t audibleFrames() const override;
    uint64_t audibleOccurrence() const;
    void waitIdle();
    std::vector<QueueRecord> snapshot() const;
    double secondsSinceStart() const { return now(); }
    double wallSecondsSinceStart() const { return now() / clockSpeed; }
    void pause() override;
    void resume() override;
    void stop() override;
    void flush() override;
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override {}
};
} // namespace queue_audio
