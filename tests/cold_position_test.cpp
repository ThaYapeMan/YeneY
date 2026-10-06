// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "timing_probe_runtime.h"
#include "timing_drift_state.h"
#include "position_state.h"
#include <cassert>
#include <fstream>
#include <regex>
#include <unistd.h>
using namespace timing_probe;
static void replay(bool prior, bool timestamp = true) {
    std::ifstream file("tests/fixtures/timing-study-a2.txt");
    std::regex expression("epoch=15.*second=[0-9]+->([0-9]+).*lo_mono=([0-9.]+) hi_mono=([0-9.]+)");
    BracketClock clock;
    if (prior) clock.usePrior(10.15, 1);
    std::string line; std::smatch m;
    double first = 0, firstSecond = 0, lock = NAN, phaseReady = NAN, countReady = NAN, previousTime = 0, maxStep = 0, maxCorrection = 0;
    ConnectionPosition p;
    uint64_t previous = 0;
    double sampleSecond = NAN, sampleTime = NAN;
    std::vector<std::pair<double,double>> reports;
    constexpr unsigned rate = 48000;
    while (std::getline(file, line)) {
        if (!std::regex_search(line, m, expression)) continue;
        double n = std::stod(m[1]), lo = std::stod(m[2]), hi = std::stod(m[3]);
        if (!first) {
            first = (lo + hi) / 2; firstSecond = n;
            p.connection(1, 1, uint64_t((first - n) * 1000));
            p.pcm(1, 1, 0, uint64_t((first - n) * 1000));
        }
        const bool reportLocked = clock.locked;
        const double reportScale = 1 / (1 + clock.drift * 1e-6), reportZero = clock.timeAt(0);
        clock.add({n, lo, hi});
        if (clock.width <= .015 && !std::isfinite(phaseReady)) phaseReady = n - firstSecond;
        if (clock.edges.size() >= 40 && !std::isfinite(countReady)) countReady = n - firstSecond;
        if (clock.locked && !std::isfinite(lock)) lock = n - firstSecond;
        // Sample at 4 Hz between edges; normal reads have a fixed 650 ms phase.
        const double end = hi;
        if (!previousTime) previousTime = first;
        for (double t = previousTime + .25; t <= end; t += .25) {
            const double relative = std::floor(t - (first - n + (n-firstSecond)) - .65);
            const double coarse = std::max(1., relative);
            p.poll(p.token(), unsigned(coarse) * 1000, uint64_t(t * 1000));
            if (coarse != sampleSecond) { sampleSecond = coarse; sampleTime = t-.65; }
            double candidate = (t - reportZero) * rate * reportScale;
            uint64_t next = p.smoothFrames(rate, t * 1000, reportLocked, candidate, reportScale,
                                           timestamp ? sampleSecond : NAN, timestamp ? sampleTime : NAN);
            reports.push_back({t, double(next) / rate});
            assert(next >= previous);
            if (previous) {
                maxStep = std::max(maxStep, double(next - previous) / rate);
                maxCorrection = std::max(maxCorrection, double(next - previous) / rate - .25);

            }
            previous = next;
            previousTime = t;
        }
        if (n - firstSecond > 180) break;
    }
    assert(std::isfinite(lock) && phaseReady == 10 && countReady == 39);
    assert(prior ? lock <= 15 : lock == 60);
    // Known SOAP timestamps keep lock convergence below the 750 ms jump
    // threshold. Unknown lease latency can exceed it: that qualified model
    // discontinuity is explicitly documented, rather than an acquisition slew.
    if (timestamp) assert(maxStep <= .288 && maxCorrection <= .038);
    else assert(maxStep <= 1.1 && maxCorrection <= .85);
    std::vector<double> beforeErrors, afterErrors, settledErrors;
    for (auto report : reports) {
        const double error = std::abs(report.second - (report.first-clock.timeAt(0))/(1+clock.drift*1e-6))*1000;
        (report.first-first < lock ? beforeErrors : afterErrors).push_back(error);
        if (report.first-first >= lock+5) settledErrors.push_back(error);
    }
    assert(percentile(settledErrors,1) <= 10);
    if (timestamp) assert(percentile(beforeErrors,1) <= 510);
    printf("PASS: Study final-fit errors prior=%d timestamp=%d before_max_ms=%.3f after_max_ms=%.3f after_p95_ms=%.3f settled_max_ms=%.3f\n",
           prior, timestamp, percentile(beforeErrors,1), percentile(afterErrors,1), percentile(afterErrors,.95), percentile(settledErrors,1));
    printf("PASS: Study cold prior=%d timestamp=%d lock_s=%.0f phase_ready_s=%.0f count_40_ready_s=%.0f largest_250ms_step_ms=%.3f no_backwards\n", prior, timestamp, lock, phaseReady, countReady, maxStep * 1000);
}
static void persistence() {
    char directory[] = "/tmp/yeney-drift-test-XXXXXX";
    assert(mkdtemp(directory));
    DriftState state; state.speaker("uuid:RINCON/example", directory);
    DriftState::Value v{10.15, 1, 1000000}, loaded;
    assert(state.save(v) && state.load(loaded, 1000001));
    assert(loaded.ppm == v.ppm && loaded.sigma == v.sigma);
    assert(!state.load(loaded, 1000000 + 8 * 86400));
    assert(!DriftState::valid({600,1,1000000},1000001));
    DriftState missing; missing.speaker("missing", std::string(directory) + "/absent");
    assert(!missing.load(loaded) && !missing.save(v));
    assert(chmod(directory, 0555) == 0);
    DriftState readonly; readonly.speaker("readonly", directory);
    assert(!readonly.load(loaded) && !readonly.save(v));
    assert(chmod(directory,0755) == 0);
    printf("PASS: atomic UDN persistence, expiry/plausibility, missing and read-only directory\n");
}
static void contradiction() {
    BracketClock c; c.usePrior(400, .5);
    for (unsigned n = 1; n <= 160; ++n) c.add({double(n), n - .001, n + .001});
    assert(c.priorDropped && !c.prior && c.driftReady && c.locked && std::abs(c.drift) < 1);
    BracketClock locked; locked.usePrior(400, .5);
    for (unsigned n=1; n<=5; ++n) locked.add({double(n),n*1.0004-.001,n*1.0004+.001});
    assert(locked.locked);
    locked.add({6,6.05,6.052}); locked.add({7,7.05,7.052});
    assert(locked.priorDropped && !locked.prior && locked.warm == 0);
    puts("PASS: contradicted prior discarded; normal live fit reacquires");
}
static double simulatedNow = 100;
static double simulatedClock() { return simulatedNow; }
static void runtimePersistence() {
    char directory[] = "/tmp/yeney-runtime-drift-XXXXXX";
    assert(mkdtemp(directory));
    setenv("YENEY_TIMING_PROBE", "1", 1);
    const std::string udn = "RINCON_runtime-test";
    Diagnostics d(simulatedClock);
    d.speaker(udn, directory); d.anchor(1,0); d.handed(1,0,48000);
    auto c = d.start(true,"persistence");
    DriftState state; state.speaker(udn,directory);
    DriftState::Value value;
    for (unsigned n=1; n<=240; ++n) {
        const double tick = 100+n*1.00001015;
        simulatedNow = tick-.001;
        d.response(c,true,simulatedNow-.001,simulatedNow+.001,n-1);
        simulatedNow = tick+.001;
        d.response(c,true,simulatedNow-.001,simulatedNow+.001,n);
        if (n==150) assert(!state.load(value));
    }
    assert(state.load(value) && std::abs(value.ppm-10.15)<2);
    d.reset("shutdown",true);
    Diagnostics warm(simulatedClock);
    warm.speaker(udn,directory);
    assert(warm.priorActive());
    warm.anchor(2,0); warm.handed(2,0,48000);
    c=warm.start(true,"warm-start");
    for (unsigned n=1; n<=5; ++n) {
        const double tick=1000+n*1.00001015;
        simulatedNow=tick-.001;
        warm.response(c,true,simulatedNow-.001,simulatedNow+.001,n-1);
        simulatedNow=tick+.001;
        warm.response(c,true,simulatedNow-.001,simulatedNow+.001,n);
    }
    double t,u;
    assert(warm.contract(5*48000,t,u) && std::abs(t-(1000+5*1.00001015))<.003);
    puts("PASS: runtime saves only mature drift; restart loads rate and reacquires a new phase in five precise edges");
}
static void quantisedAcquisition() {
    ConnectionPosition p;
    p.connection(1,1,100000); p.pcm(1,1,0,100000);
    p.poll(p.token(),2000,102600);
    const auto before = p.smoothFrames(48000,102600,false,0);
    // Leased one-second polling can cross a tick and skip an integer.
    p.poll(p.token(),4000,103600);
    const auto after = p.smoothFrames(48000,103600,false,0);
    assert(after >= before && double(after)/48000 >= 4 && double(after)/48000 <= 5);
    puts("PASS: skipped quantised RelTime tick re-anchors only to the latest interval");
}
static void reconnectFallback() {
    ConnectionPosition p;
    p.connection(1,1,100000); p.pcm(1,1,0,100000);
    p.poll(p.token(),2000,102600);
    p.smoothFrames(48000,102600,false,0);
    // Reconnect without start lead: history is retained for the audible base,
    // but its old RelTime timestamp must not seed the new connection.
    p.connection(1,2,112600); p.pcm(1,2,3*48000,112600,false);
    const auto fallback = p.audibleFrames(48000);
    assert(p.smoothFrames(48000,122600,false,0) == fallback);
    puts("PASS: reconnect holds the existing fallback until a fresh positive RelTime observation");
}
static void seek() {
    ConnectionPosition p; p.connection(1,1,100000); p.pcm(1,1,0,100000);
    p.poll(p.token(),2000,102600);
    p.smoothFrames(48000,102600,false,0);
    p.smoothFrames(48000,102850,true,3.4*48000);
    assert(std::string(p.phase(false)) == "slewing");
    p.reset(2); p.connection(2,2,103000); p.pcm(2,2,0,103000);
    p.poll(p.token(),20000,124000);
    auto selected = p.smoothFrames(48000,124000,false,0);
    assert(selected == uint64_t(20.5*48000));
    assert(std::string(p.phase(false)) == "acquiring");
    puts("PASS: seek during slew resets offset and jumps to new coordinate");
}
int main() { replay(false); replay(true); replay(false,false); persistence(); contradiction(); quantisedAcquisition(); reconnectFallback(); seek(); runtimePersistence(); }
