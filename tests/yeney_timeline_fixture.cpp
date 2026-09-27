#include "clock.h"
#include <resume_state.h>
#include "sbstreamer.h"
#include "sonos-position.h"
#include "upnp/speaker_state.h"
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <future>
#include <thread>
static std::atomic<unsigned> streamId{30}, plays{0}, logs{0};
static std::atomic<bool> ourStreamStarted{true};
static ResumeState resumeState;
static std::mutex resumeMutex;
static upnp::SpeakerStateStore speakerState;
struct Player { upnp::TransportInfo transportInfo() { return speakerState.snapshot().transport; } } player;
static Player* gPlayer = &player;
static int gServer, gMac;
static bool stream_just_restarted() { return false; }
static bool sendLmsCommand(int, int, const char* text) { if (std::string(text) == "play") ++plays; return true; }
extern "C" {
unsigned get_squeezebox_stream_id() { return streamId; }
unsigned get_lms_stream_serial() { return 1; }
int sonos_lms_is_paused() { return 1; }
int sonos_output_running() { return 1; }
uint64_t get_sb_time_ms() { return TimelineClock::ms; }
void hold_squeezebox_resume(unsigned);
void end_squeezebox_response();
void flush_squeezebox_response();
void note_squeezebox_device_close();
}
std::string SqueezeBoxURL(unsigned id) { return "http://bridge/stream?stream=" + std::to_string(id); }
static int resumePrintf(const char* format, ...) {
    char buf[1024]; va_list args; va_start(args, format);
    const int n = vsnprintf(buf, sizeof(buf), format, args); va_end(args);
    if (std::string(buf).find("Device-initiated resume: current stream") == 0) ++logs;
    fputs(buf, stdout); return n;
}
#define printf resumePrintf
#include "production_resume.inc"
#undef printf
#include "stream_session.h"
class Request : public upnp::StreamRequest {
public:
    std::atomic<bool> entered{false}, finished{false};
    std::string wire;
    std::string path() const override { return "/music/squeezebox.flac"; }
    Method method() const override { return Method::Get; }
    std::string parameter(const std::string& name) const override { return name == "session" ? streamSessionToken() : "30"; }
    bool send(const char* p, size_t n) override { wire.append(p, n); return true; }
    bool peerClosed() override { entered = true; return false; }
    void sendTimeout(unsigned) override {}
    void disconnect() override {}
    void reply(unsigned) {}
    void reply(unsigned, const std::string&) override {}
    std::string serverName() const override { return "fixture"; }
};
template<class F> static void wait(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!condition()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
int main(int argc, char** argv) {
    assert(argc == 3);
    const int eventAt = std::stoi(argv[1]); const bool two = std::stoi(argv[2]);
    bridge::SBStreamer streamer;
    resumeState.command('q'); resumeState.stopForPause(30);
    upnp::StateUpdate update; update.source = upnp::Source::Event; update.state = "STOPPED";
    speakerState.apply(update); ObserveDeviceTransport("STOPPED");
    auto event = [&] { update.state = "TRANSITIONING"; speakerState.apply(update); ObserveDeviceTransport("TRANSITIONING"); };
    if (eventAt < 0) event();
    Request first, second;
    auto serve = [&](Request& r) { streamer.HandleRequest(&r); r.finished = true; };
    auto one = std::async(std::launch::async, [&] { serve(first); });
    wait([&] { return first.entered.load(); });
    const auto firstToken = sonos_position_poll_token();
    std::future<void> other;
    if (two) {
        TimelineClock::ms = 3;
        other = std::async(std::launch::async, [&] { serve(second); });
        wait([&] { return second.entered.load(); });
    }
    if (eventAt > 5000) {
        TimelineClock::ms = 5000;
        wait([&] { return first.finished.load(); });
        assert(first.wire.find("503") != std::string::npos);
        // Wait for the real standby loop to publish its new position owner.
        wait([&] { return sonos_position_poll_token() != firstToken; });
        assert(!second.finished && plays == 0);
    }
    TimelineClock::ms = eventAt < 0 ? 0 : eventAt;
    if (eventAt >= 0) event();
    wait([&] { return plays.load() == 1; });
    assert(logs == 1);
    if (eventAt <= 5000) { assert(!first.finished); if (two) { assert(!second.finished); assert(sonos_position_poll_token() == firstToken); } }
    // Model LMS's q/s response: preserve the marked held GET, then start a new stream.
    { std::lock_guard<std::mutex> lock(resumeMutex); resumeState.command('q'); }
    flush_squeezebox_response();
    streamId = 31;
    wait([&] { return first.finished.load() && (!two || second.finished.load()); });
    one.get(); if (two) other.get();
    const auto& resumed = eventAt > 5000 ? second : first;
    assert(resumed.wire.find("302 Found") != std::string::npos);
    assert(resumed.wire.find("503") == std::string::npos);
    assert(plays == 1 && logs == 1);
    printf("PASS: timeline event=%d ms standby=%d: one Device-initiated resume, one LMS play, held GET redirects without 503\n", eventAt, two);
}
