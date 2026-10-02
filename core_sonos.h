#pragma once
#include "third_party/yeney-core/core/player.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// Only the feeder may wait for an HTTP consumer. Event-loop callbacks only
// copy bounded batches and update state under short-lived locks.
class CoreSonosSink : public yeney::Sink {
    struct Batch {
        std::vector<char> pcm;
        uint64_t first, generation;
        size_t frames;
        bool boundary, continuous;
        unsigned rate;
    };
    static constexpr size_t capacity = 48000;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Batch> queue;
    std::deque<bool> starts;
    std::thread feeder;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> generation{0};
    size_t staged = 0;
    bool paused = false, pending = false, continuous = false, inflight = false;
    unsigned rate = 44100, streamRate = 0;
    uint64_t next = 0, streamBase = 0, handed = 0, started = 0;
    mutable uint64_t audible = 0;
    void feed();
public:
    CoreSonosSink();
    ~CoreSonosSink() override;
    void command(const yeney::Command&);
    uint32_t maxSampleRate() const override;
    void trackBoundary(uint64_t, const yeney::Format&, bool) override;
    size_t write(const yeney::Frame*, size_t) override;
    void pause() override;
    void resume() override;
    void stop() override;
    void flush() override;
    uint64_t audibleFrames() const override;
    uint64_t startedFrames() const override;
    bool drained(uint64_t) const override;
    bool outputEmpty(uint64_t) const override;
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override {}
    bool paced() const override { return false; }
};
void runCoreClient(const char* server, const uint8_t* mac, const char* name);
extern "C" int sonos_output_running();
