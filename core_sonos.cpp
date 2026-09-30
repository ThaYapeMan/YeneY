// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core_sonos.h"
#include "compatibility_decoder.h"
#include "audio_mode.h"
#include "sonos-position.h"
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

extern "C" {
void yeney_transport(char);
unsigned get_squeezebox_stream_id();
void new_squeezebox_stream_id();
int encode_squeezebox_audio_cancellable(const char*, int, uint64_t, int (*)(void*), void*);
}
static std::atomic<bool> coreRunning{false}, coreStop{false};
extern "C" int core_output_running() { return coreRunning.load(); }
static void stopCore(int sig) { coreStop.store(true); std::signal(sig, SIG_DFL); }
CoreSonosSink::CoreSonosSink() {
    coreRunning.store(true);
    feeder = std::thread(&CoreSonosSink::feed, this);
}
CoreSonosSink::~CoreSonosSink() {
    running.store(false);
    coreRunning.store(false);
    ++generation;
    changed.notify_all();
    feeder.join();
}
uint32_t CoreSonosSink::maxSampleRate() const { return sonos_audio_legacy() ? 44100 : 48000; }
void CoreSonosSink::command(const yeney::Command& c) {
    bool continuation = c.letter == 's' && !sonos_audio_legacy() && c.playing;
    if (c.letter == 's') starts.push_back(continuation);
    if (!continuation && (c.letter != 'p' || c.value == 0)) yeney_transport(c.letter);
}
void CoreSonosSink::trackBoundary(uint64_t frame, const yeney::Format& f, bool) {
    std::lock_guard<std::mutex> lock(mutex);
    next = frame; rate = sonos_audio_legacy() ? 44100 : f.rate;
    pending = true;
    continuous = !starts.empty() && starts.front();
    if (!starts.empty()) starts.pop_front();
}
size_t CoreSonosSink::write(const yeney::Frame* frames, size_t count) {
    std::lock_guard<std::mutex> lock(mutex);
    if (paused || !running.load()) return 0;
    count = std::min(count, capacity - staged);
    if (!count) return 0;
    const unsigned bytes = sonos_audio_legacy() ? 2 : 3;
    Batch b{{}, next, generation.load(), count, pending, continuous, rate};
    b.pcm.reserve(count * bytes * 2);
    for (size_t i = 0; i < count; ++i)
        for (uint32_t sample : {uint32_t(frames[i].left), uint32_t(frames[i].right)})
            for (unsigned j = 4 - bytes; j < 4; ++j) b.pcm.push_back(char(sample >> (j * 8)));
    queue.push_back(std::move(b));
    staged += count; next += count; pending = false;
    changed.notify_one();
    return count;
}
void CoreSonosSink::pause() { std::lock_guard<std::mutex> lock(mutex); paused = true; }
void CoreSonosSink::resume() { std::lock_guard<std::mutex> lock(mutex); paused = false; changed.notify_one(); }
void CoreSonosSink::stop() { pause(); flush(); }
void CoreSonosSink::flush() {
    const auto position = audibleFrames();
    std::lock_guard<std::mutex> lock(mutex);
    ++generation;
    queue.clear(); starts.clear(); staged = 0; pending = false;
    handed = next = streamBase = started = position;
    streamRate = 0;
    changed.notify_one();
}
uint64_t CoreSonosSink::audibleFrames() const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!paused && streamRate)
        audible = std::max(audible, std::min(handed, streamBase + get_sonos_audible_frames(streamRate)));
    return audible;
}
uint64_t CoreSonosSink::startedFrames() const {
    std::lock_guard<std::mutex> lock(mutex); return started;
}
bool CoreSonosSink::drained(uint64_t) const {
    std::lock_guard<std::mutex> lock(mutex);
    return queue.empty() && !inflight;
}
void CoreSonosSink::feed() {
    while (running.load()) {
        Batch b;
        uint64_t first;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return !running.load() || (!paused && !queue.empty()); });
            if (!running.load()) break;
            b = std::move(queue.front()); queue.pop_front();
            staged -= b.frames; inflight = true;
            if (b.boundary && (!b.continuous || streamRate != b.rate || sonos_audio_legacy())) {
                set_squeezebox_audio_rate(get_squeezebox_stream_id() + 1, b.rate);
                new_squeezebox_stream_id();
                streamRate = b.rate; streamBase = b.first;
                printf("stream %u: FLAC %u-bit %u Hz\n", get_squeezebox_stream_id(),
                       sonos_audio_legacy() ? 16u : 24u, b.rate);
            }
            first = b.first - streamBase;
            started = std::max(started, b.first + b.frames);
        }
        struct Cancellation { CoreSonosSink* sink; uint64_t generation; } cancel{this, b.generation};
        int ok = encode_squeezebox_audio_cancellable(b.pcm.data(), b.pcm.size(), first, [](void* ptr) {
            auto& c = *static_cast<Cancellation*>(ptr);
            return int(!c.sink->running.load() || c.sink->generation.load() != c.generation);
        }, &cancel);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (ok && generation.load() == b.generation) handed = std::max(handed, b.first + b.frames);
            inflight = false;
        }
    }
}
void runCoreClient(const char* server, const uint8_t* mac, const char* name) {
    for (int sig : {SIGINT, SIGTERM, SIGQUIT, SIGHUP}) std::signal(sig, stopCore);
    try {
        CoreSonosSink sink;
        yeney::Config config;
        config.server = server ? server : "";
        auto colon = config.server.find(':');
        if (colon != std::string::npos) {
            auto port = std::stoul(config.server.substr(colon + 1));
            if (!port || port > 65535 || port == 9000) throw std::runtime_error("invalid Slimproto port");
            config.port = port; config.server.resize(colon);
        }
        config.name = name;
        std::copy(mac, mac + 6, config.mac.begin());
        config.log = [](const std::string& line) { printf("core: %s\n", line.c_str()); };
        config.observeCommand = [&](const yeney::Command& c) { sink.command(c); };
        config.startOnSubmit = true;
        config.decoderFactory = makeCompatibilityDecoder;
        yeney::Player player(config, sink);
        player.run(coreStop);
    } catch (const std::exception& error) {
        printf("core: fatal: %s\n", error.what());
        std::exit(1);
    }
    std::exit(0); // Same atexit shutdown path as the squeezelite engine.
}
