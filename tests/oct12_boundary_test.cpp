// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Replay captured SOAP/STAT times through the production sink and position state.
#include "core_sonos.h"
#include "position_state.h"
#include "third_party/yeney-core/core/play_clock.h"
#include "timing_probe_runtime.h"
#include <cassert>
#include <fstream>
#include <iomanip>
#include <regex>
#include <thread>
#include <unistd.h>
static std::atomic<uint64_t> nowMs{0};
static double clockSeconds() { return double(nowMs.load()) / 1000; }
static uint64_t clockMs() { return nowMs.load(); }
static unsigned stream;
static ConnectionPosition position;
static timing_probe::Diagnostics *diagnostic;
static double oldAt, oldElapsed, scale;
extern "C" int sonos_audio_legacy() { return 0; }
extern "C" void yeney_transport(char) {}
extern "C" unsigned get_squeezebox_stream_id() { return stream; }
extern "C" void set_squeezebox_audio_rate(unsigned, unsigned) {}
extern "C" void new_squeezebox_stream_id() {
    ++stream;
    position.reset(stream);
    position.connection(stream, 1, clockMs());
    position.pcm(stream, 1, 0, clockMs());
}
extern "C" void sonos_position_allow_zero_start() { position.allowZeroStart(); }
extern "C" int sonos_position_audio_started() {
    return position.audioStarted() ||
           (diagnostic && (position.needsStartEvidence() ? diagnostic->playbackConfirmed(stream)
                                                         : diagnostic->heardAudio(stream)));
}
extern "C" uint64_t get_sonos_audible_frames(uint32_t rate) {
    if (stream == 1)
        return uint64_t((oldElapsed + (clockSeconds() - oldAt) * scale) * rate);
    double zeroTime;
    if (position.needsStartEvidence() && diagnostic->playbackConfirmed(stream) &&
        diagnostic->startupZero(stream, zeroTime))
        position.startZero(zeroTime * 1000);
    uint32_t ms;
    double time;
    if (position.needsStartEvidence() && diagnostic->startupReading(stream, ms, time))
        position.poll(position.token(), ms, uint64_t(time * 1000));
    double candidate = 0, speed = scale, second = NAN, observed = NAN;
    bool model = diagnostic->lmsPosition(stream, position.measurementBase(), rate, candidate, speed, &second,
                                         &observed);
    return position.smoothFrames(rate, double(clockMs()), model, candidate, speed, second, observed);
}
extern "C" int encode_squeezebox_audio_cancellable(const char *, int, uint64_t, int (*)(void *), void *) {
    return 1;
}
static void drain(CoreSonosSink &sink) {
    for (unsigned i = 0; i < 10000 && !sink.drained(0); ++i)
        usleep(100);
    assert(sink.drained(0));
}
static void submit(CoreSonosSink &sink, uint64_t frames) {
    std::vector<yeney::Frame> batch(48000);
    while (frames) {
        const size_t n = std::min<uint64_t>(batch.size(), frames);
        const auto accepted = sink.write(batch.data(), n);
        frames -= accepted;
        drain(sink);
    }
}
static double wall(const std::string &s) {
    return std::stod(s.substr(7, 2)) * 3600 + std::stod(s.substr(10, 2)) * 60 + std::stod(s.substr(13, 9));
}
static void replay(const char *kind) {
    std::ifstream file(std::string("tests/fixtures/oct12-boundary/") + kind + "-journal.txt");
    assert(file);
    std::vector<std::string> lines;
    std::string line;
    while (getline(file, line))
        lines.push_back(line);
    std::regex stat("core: (STMt|STMs) jiffies=([0-9]+) elapsed_ms=([0-9]+)");
    std::regex display("\\| +([0-9]+):([0-9]+):([0-9]+) /");
    std::regex raw("stream=2 epoch=([0-9]+).*send_mono=([0-9.]+) "
                   "recv_mono=([0-9.]+).*reltime_s=([0-9.]+).*outcome=accepted");
    std::smatch m;
    double offset = 0, boundary = 0, start = 0, create = 0;
    uint64_t end = 0;
    for (const auto &l : lines) {
        if (regex_search(l, m, stat) && !oldAt) {
            oldAt = stod(m[2]) / 1000;
            oldElapsed = stod(m[3]) / 1000;
            offset = oldAt - wall(l);
        }
        if (l.find("core: boundary frame=28371728") != std::string::npos) {
            boundary = wall(l) + offset;
            end = 28371728;
        }
        if (l.find("Creating new stream (2)") != std::string::npos)
            create = wall(l) + offset;
        if (regex_search(l, m, stat) && m[1] == "STMs")
            start = stod(m[2]) / 1000;
    }
    assert(end && create > boundary && start > create);
    scale = 1 / (1 + (std::string(kind) == "cold" ? 10.6 : 10.1) * 1e-6);
    nowMs = uint64_t(oldAt * 1000);
    stream = 0;
    CoreSonosSink sink(nullptr, clockMs);
    sink.command({'s', 0, false});
    sink.trackBoundary(0, {48000, 24, 2, false}, false);
    submit(sink, end);
    assert(stream == 1);
    char directory[] = "/tmp/yeney-boundary-XXXXXX";
    assert(mkdtemp(directory));
    timing_probe::DriftState saved;
    saved.speaker("boundary", directory);
    assert(saved.save({(1 / scale - 1) * 1e6, 7, ::time(nullptr)}));
    timing_probe::Diagnostics d(clockSeconds);
    diagnostic = &d;
    d.speaker("boundary", directory);
    bool created = false, announcing = false, timer = false;
    double reportAt = oldAt, reported = oldElapsed, nextSample = oldAt;
    unsigned reportTrack = 1;
    uint64_t epoch = 0;
    timing_probe::Context context;
    std::ofstream result(std::string("/tmp/oct12-") + kind + "-replay.txt");
    size_t cursor = 1;
    double firstStart = 0;
    // Preserve field response order and times; query reporting at 250 ms.
    std::vector<std::pair<double, double>> before, after;
    for (double t = oldAt; t <= start + 10.001; t += .002) {
        while (cursor < lines.size() && wall(lines[cursor]) + offset <= t) {
            const auto &l = lines[cursor++];
            nowMs = uint64_t((wall(l) + offset) * 1000);
            if (l.find("Creating new stream (2)") != std::string::npos) {
                sink.command({'s', 0, true});
                sink.trackBoundary(end, {44100, 24, 2, false}, false);
                submit(sink, 44100 * 20);
                d.anchor(2, 0);
                d.handed(2, 44100 * 20, 44100);
                created = true;
            }
            if (regex_search(l, m, stat) && m[1] == "STMt")
                timer = true;
            if (created && l.find("TRANSITIONING") != std::string::npos) {
                d.start(false, "boundary");
                // Normal polling also returns zero during startup; it must not
                // seed the reporting clock before eligible playback evidence.
                position.poll(position.token(), 0, clockMs());
            }
            // The status panel exposes normal leased counter values, but not
            // their original SOAP timestamps. Use its journal time explicitly
            // as an approximate normal observation, retaining repeated leases.
            if (created && l.find("PLAYING") != std::string::npos && regex_search(l, m, display)) {
                const uint32_t ms =
                    (std::stoul(m[1]) * 3600 + std::stoul(m[2]) * 60 + std::stoul(m[3])) * 1000;
                position.poll(position.token(), ms, clockMs());
            }
            if (created && regex_search(l, m, raw)) {
                const auto inputEpoch = std::stoull(m[1]);
                if (inputEpoch != epoch) {
                    d.start(false, "boundary");
                    context = d.start(true, "boundary");
                    epoch = inputEpoch;
                }
                nowMs = uint64_t(stod(m[3]) * 1000);
                d.response(context, true, stod(m[2]), stod(m[3]), stod(m[4]));
            }
        }
        nowMs = uint64_t(t * 1000);
        const auto started = sink.startedFrames();
        bool startedNow = created && !announcing && started > end;
        if (startedNow) {
            announcing = true;
            firstStart = (sink.audibleFrames() - end) / 44100.;
        }
        if (timer || startedNow) {
            const auto frame = sink.audibleFrames();
            reported = yeney::trackElapsedMs(frame, announcing ? end : 0, announcing ? UINT64_MAX : end, 0,
                                             announcing ? 44100 : 48000) /
                       1000.;
            assert(announcing || reported <= double(end) / 48000);
            reportAt = t;
            reportTrack = announcing ? 2 : 1;
            timer = false;
        }
        if (t + .00001 < nextSample)
            continue;
        // LMS interpolates each STAT at unity, then clamps to library duration.
        double elapsed = reported + (t - reportAt);
        if (reportTrack == 1)
            elapsed = std::min(elapsed, 591.090);
        (reportTrack == 2 ? after : before).push_back({t, elapsed});
        result << std::fixed << std::setprecision(9) << t << ' ' << elapsed << ' ' << reportTrack << '\n';
        nextSample += .25;
    }
    assert(!after.empty() && firstStart > 0 && firstStart < .3);
    double excess = 0, backwards = 0, flat = 0, longest = 0;
    for (const auto *group : {&before, &after})
        for (size_t i = 1; i < group->size(); ++i) {
            const double dp = (*group)[i].second - (*group)[i - 1].second,
                         dt = (*group)[i].first - (*group)[i - 1].first;
            excess = std::max(excess, dp - dt);
            backwards = std::max(backwards, -dp);
            flat = dp == 0 ? flat + dt : 0;
            longest = std::max(longest, flat);
        }
    printf("October 12 %s production replay: STMs=%.3f s excess=%.3f ms backwards=%.3f ms flat=%.3f s (gap "
           "unknown)\n",
           kind, firstStart, excess * 1000, backwards * 1000, longest);
    assert(excess <= .100 && backwards == 0 && longest <= .5);
    // Old-clock end hold is reported separately; it overlaps the observed
    // TRANSITIONING/startup period and cannot establish an acoustic gap.
    for (const auto &sample : before)
        assert(sample.second <= 591.090);
    diagnostic = nullptr;
    oldAt = 0;
}
static void positiveOnlyStart() {
    ConnectionPosition p;
    p.reset(2);
    p.allowZeroStart();
    p.connection(2, 1, 100000);
    p.pcm(2, 1, 0, 100000);
    p.poll(p.token(), 0, 101200);
    assert(p.smoothFrames(44100, 101200, false, 0) == 0);
    p.poll(p.token(), 1000, 102000);
    assert(p.smoothFrames(44100, 102000, false, 0) == 44100);
    assert(p.smoothFrames(44100, 102250, false, 0) == 55125);
    assert(p.smoothFrames(44100, 103000, false, 0) == 88200);
    puts("PASS: unqualified zero is held; positive-only startup seeds the lower endpoint and advances at "
         "unity");
}

static void pauseDuringHandoff() {
    stream = 0;
    nowMs = 100000;
    oldAt = 100;
    oldElapsed = .05;
    scale = 1;
    diagnostic = nullptr;
    CoreSonosSink sink(nullptr, clockMs);
    sink.command({'s', 0, false});
    sink.trackBoundary(0, {48000, 24, 2, false}, false);
    submit(sink, 48000);
    nowMs = 100100;
    sink.command({'s', 0, true});
    sink.trackBoundary(48000, {44100, 24, 2, false}, false);
    submit(sink, 44100);
    nowMs = 100200;
    sink.pause();
    const auto held = sink.audibleFrames();
    nowMs = 110200;
    assert(sink.audibleFrames() == held);
    sink.resume();
    assert(sink.audibleFrames() == held);
    nowMs = 110450;
    assert(sink.audibleFrames() == held + 12000);
    sink.flush();
    assert(sink.audibleFrames() == held + 12000);
    oldAt = 0;
    puts("PASS: pause/resume/flush during rate-change handoff retain the old clock without counting paused "
         "time");
}

int main() {
    setenv("YENEY_START_LEAD_MS", "0", 1);
    setenv("YENEY_TIMING_PROBE", "1", 1);
    positiveOnlyStart();
    pauseDuringHandoff();
    replay("cold");
    replay("warm");
}
