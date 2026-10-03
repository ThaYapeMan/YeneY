// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "timing_probe_runtime.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <random>
#include <regex>
#include <thread>
using namespace timing_probe;
static double now = 0;
static double fakeClock() { return now; }
static void field() {
    std::ifstream file("tests/fixtures/timing-study-a2.txt");
    assert(file);
    const std::regex expression(
        "epoch=([0-9]+).*second=[0-9]+->([0-9]+) lo_mono=([0-9.]+) hi_mono=([0-9.]+)");
    std::string line;
    BracketClock c;
    unsigned count = 0, epoch = 0;
    double first = 0, locked = NAN;
    while (std::getline(file, line)) {
        std::smatch m;
        assert(std::regex_search(line, m, expression));
        unsigned next = std::stoul(m[1]);
        if (next != epoch) {
            epoch = next;
            c.reset();
            count = 0;
            first = std::stod(m[2]);
            locked = NAN;
        }
        c.add({std::stod(m[2]), std::stod(m[3]), std::stod(m[4])});
        ++count;
        if (c.locked && !std::isfinite(locked))
            locked = std::stod(m[2]) - first;
    }
    assert(epoch == 15 && count == 1232 && locked <= 120 && c.locked);
    assert(c.drift >= 5.5 && c.drift <= 16 && c.width <= .006 && c.violators <= c.edges.size() * .01 &&
           c.uncertainty() <= 5);
    printf("PASS: Study replay edges=%u lock_s=%.0f drift_ppm=%.3f band_ms=%.3f violators=%u "
           "uncertainty_ms=%.3f\n",
           count, locked, c.drift, c.width * 1000, c.violators, c.uncertainty());
}
static void simulation() {
    for (unsigned seed : {17u, 31u, 89u})
        for (double ppm : {0., -200., 200.}) {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<double> random(0, 1);
            BracketClock c;
            std::vector<double> error;
            unsigned hits = 0;
            for (unsigned n = 1; n <= 600; ++n) {
                if (random(rng) < .025)
                    continue;
                const double truth = 100 + n * (1 + ppm * 1e-6);
                // Field-style delayed observations widen brackets, rather than move the physical tick.
                double lo = truth - (.002 + random(rng) * .005), hi = truth + (.002 + random(rng) * .005);
                if (random(rng) < .1)
                    hi += .1 + random(rng) * .2;
                c.add({double(n), lo, hi});
                if (c.locked) {
                    error.push_back(std::abs(c.timeAt(n) - truth) * 1000);
                    ++hits;
                }
            }
            assert(c.locked && hits > 300 && percentile(error, .95) <= 5 && std::abs(c.drift - ppm) <= 3);
            printf(
                "PASS: bracket simulation seed=%u truth_ppm=%.0f p95_ms=%.3f drift_ppm=%.3f brackets=%zu\n",
                seed, ppm, percentile(error, .95), c.drift, c.edges.size());
        }
    BracketClock warm;
    warm.reset(200);
    for (unsigned n = 1; n <= 100; ++n)
        warm.add({double(n), n * 1.0002 - .003, n * 1.0002 + .003});
    assert(warm.drift == 0 && !warm.driftReady && warm.warm == 200);
    for (unsigned n = 101; n <= 240; ++n)
        warm.add({double(n), n * 1.0002 - .003, n * 1.0002 + .003});
    assert(warm.locked && std::abs(warm.drift - 200) <= 3);
    puts("PASS: qualified warm drift seeds reacquisition; slope remains unity until 120 s");
    BracketClock bad;
    for (unsigned n = 1; n <= 600; ++n) {
        double delay = n % 10 == 0 ? .2 : 0;
        bad.add({double(n), n + delay - .003, n + delay + .003});
    }
    assert(!bad.locked);
    puts("PASS: 10% physically inconsistent ticks fail closed (2% contract budget)");
    BracketClock step;
    for (unsigned n = 1; n <= 150; ++n)
        step.add({double(n), n - .003, n + .003});
    assert(step.locked);
    step.add({151, 151.047, 151.053});
    assert(step.locked);
    step.add({152, 152.047, 152.053});
    assert(!step.locked && step.edges.size() == 1);
    for (unsigned n = 153; n <= 300; ++n)
        step.add({double(n), n + .05 - .003, n + .05 + .003});
    assert(step.locked && std::abs(step.timeAt(300) - 300.05) < .005);
    puts("PASS: two mismatches reacquire a 50 ms step without publishing its first noisy tick");
    for (unsigned n = 301; n <= 1200; ++n) {
        double truth = n + .05 + (n - 300) * 30e-6;
        step.add({double(n), truth - .001, truth + .001});
    }
    assert(step.locked && std::abs(step.drift - 30) < 3);
    puts("PASS: 30 ppm rate change reacquires and converges");
}
static void mapping() {
    FrameMapping m;
    uint64_t abs;
    assert(m.add(3, 44100, 7, 0, 1000, 44100));
    assert(!m.add(3, 44100, 7, 44100, 45100, 44100)); // gapless
    assert(m.absolute(88200, abs) && abs == 89200);   // HTTP Range keeps canonical PCM coordinates
    assert(m.absolute(60000, abs) && abs == 61000);   // reconnect anchor into same run
    assert(m.add(4, 48000, 7, 0, 89200, 48000));
    assert(m.rate == 48000 && m.valid_from == 89200 && m.absolute(48000, abs) && abs == 137200);
    assert(m.add(4, 48000, 8, 48000, 0, 48000));
    assert(!m.absolute(0, abs));
    assert(m.add(4, 48000, 8, 96000, 48001, 256)); // skipped export -> a new affine run
    assert(!m.absolute(95000, abs));
    puts("PASS: exact PCM/SHM affine coordinates: gapless, Range, reconnect, rate, generation, export gap");
}
static void seqlock() {
    TimingRecord memory;
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        for (unsigned n = 1; n < 50000; ++n) {
            TimingRecord r;
            r.model_epoch = n;
            r.anchor_abs_frame = n * 17;
            r.shm_generation = n * 23;
            storeRecord(&memory, r);
        }
        finished = true;
    });
    unsigned reads = 0;
    do {
        TimingRecord r;
        if (loadRecord(&memory, r)) {
            assert(r.anchor_abs_frame == r.model_epoch * 17 && r.shm_generation == r.model_epoch * 23);
            ++reads;
        }
    } while (!finished);
    writer.join();
    assert(reads > 0);
    puts("PASS: 128-byte LE layout, concurrent seqlock reader rejects torn records");
}
static void polling() {
    now = 100;
    Diagnostics d(fakeClock);
    d.anchor(3, 0);
    d.handed(3, 0, 44100);
    unsigned lockedRequests = 0;
    double lockedStart = 0;
    std::mt19937 rng(18);
    // Requests are simulated on the same dedicated-worker API as production.
    std::vector<double> error;
    double drift = 0;
    for (now = 100; now < 750; now += .002) {
        auto c = d.start(true, "simulation");
        if (!d.request(c, now))
            continue;
        if (now > 250) {
            ++lockedRequests;
            if (!lockedStart)
                lockedStart = now;
        }
        double rtt = .004 + (rng() % 1600) * .00001;
        if (rng() % 100 == 0)
            rtt = .060;
        double sent = now, received = now + rtt;
        double second = std::floor((sent + received) / 2 - 100);
        d.response(c, true, sent, received, second);
        now = received;
        double predicted, uncertainty;
        if (d.contract(uint64_t(second) * 44100, predicted, uncertainty, &drift))
            error.push_back(std::abs(predicted - (100 + second)) * 1000);
    }
    double rps = double(lockedRequests) / (750 - 250);
    assert(rps > .2 && rps <= .8 && error.size() > 100 && percentile(error, .95) <= 5 &&
           std::abs(drift) <= 3);
    printf("PASS: locked worker truth p95_ms=%.3f drift_ppm=%.3f\n", percentile(error, .95), drift);
    printf("PASS: locked polling actual worker scheduler rps=%.3f (RTT 4-20 ms, rare 60 ms)\n", rps);
}
static void bracketFreshness() {
    now = 100;
    Diagnostics d(fakeClock);
    d.anchor(3, 0);
    d.handed(3, 0, 44100);
    auto c = d.start(true, "wide-brackets");
    for (unsigned n = 1; n <= 250; ++n) {
        now = 100 + n - .004;
        d.response(c, true, now, now + .002, n - 1);
        now = 100 + n + (n <= 150 ? .002 : .300);
        d.response(c, true, now, now + .002, n);
        d.request(c, now);
        assert(d.snapshot().active && d.snapshot().epoch == c.epoch);
    }
    double t, u;
    assert(d.contract(250 * 44100, t, u));
    puts("PASS: valid wide brackets keep the contract alive despite >60 s of midpoint rejections");
}
static void publication() {
    char mac[32];
    snprintf(mac, sizeof mac, "02:00:00:01:%02x:%02x", unsigned(getpid()) >> 8 & 255,
             unsigned(getpid()) & 255);
    std::string path = "/yeney-timing-" + std::string(mac);
    now = 100;
    Diagnostics d(fakeClock);
    d.configure(mac);
    int fd = shm_open(path.c_str(), O_RDONLY, 0);
    assert(fd >= 0);
    auto *shared = static_cast<const TimingRecord *>(mmap(nullptr, 128, PROT_READ, MAP_SHARED, fd, 0));
    assert(shared != MAP_FAILED);
    TimingRecord r;
    assert(loadRecord(shared, r) && r.state == 0);
    {
        Publisher duplicate;
        assert(!duplicate.open(mac));
    }
    assert(loadRecord(shared, r) && r.state == 0);
    d.exported(7, 1000 + 44100 * 130);
    d.mapFrames(3, 44100, 7, 0, 1000, 44100 * 130);
    d.anchor(3, 0);
    d.handed(3, 0, 44100);
    auto c = d.start(true, "publication");
    for (unsigned n = 1; n <= 130; ++n) {
        now = 100 + n - .006;
        d.response(c, true, now, now + .004, n - 1);
        now = 100 + n + .002;
        d.response(c, true, now, now + .004, n);
        d.request(c, now); // heartbeat and state changes, no actual network calls
    }
    assert(loadRecord(shared, r) && r.state == 2 && r.shm_generation == 7 && r.sample_rate_hz == 44100);
    assert(r.anchor_abs_frame == 1000 && r.uncertainty_us <= 7500 && r.offset_us == audibleOffset() * 1000);
    assert(std::abs(double(r.anchor_audible_mono_ns) / 1e9 - (100 + audibleOffset() * .001)) < .005);
    uint64_t epoch = r.model_epoch;
    auto seq = r.write_seq;
    d.request(c, now + .001);
    assert(loadRecord(shared, r) && r.write_seq == seq);
    d.exported(8, 1000);
    assert(loadRecord(shared, r) && r.state == 1 && r.model_epoch > epoch && r.shm_generation == 8);
    d.mapFrames(3, 44100, 8, 130 * 44100, 0, 1000);
    assert(loadRecord(shared, r) && r.state == 1 && r.model_epoch > epoch && r.shm_generation == 8 &&
           !r.anchor_audible_mono_ns);
    c = d.snapshot();
    for (unsigned n = 131; n <= 260; ++n) {
        now = 100 + n - .006;
        d.response(c, true, now, now + .004, n - 1);
        now = 100 + n + .002;
        d.response(c, true, now, now + .004, n);
        d.request(c, now);
    }
    assert(loadRecord(shared, r) && r.state == 2 && r.anchor_abs_frame == 0 &&
           r.valid_from_abs_frame >= 1000);
    assert(std::abs(double(r.anchor_audible_mono_ns) / 1e9 - (230 + audibleOffset() * .001)) < .005);
    epoch = r.model_epoch;
    d.reset("pause", false, true);
    assert(loadRecord(shared, r) && r.state == 0 && r.model_epoch > epoch && r.discontinuity == 1);
    epoch = r.model_epoch;
    d.mapFrames(4, 48000, 8, 0, 0, 48000);
    d.anchor(4, 0);
    d.handed(4, 0, 48000);
    d.start(true, "publication");
    assert(loadRecord(shared, r) && r.state == 1 && r.model_epoch > epoch && r.shm_generation == 8 &&
           !r.anchor_audible_mono_ns);
    d.start(false, "member");
    assert(loadRecord(shared, r) && r.state == 0);
    d.reset("shutdown", true);
    assert(shm_open(path.c_str(), O_RDONLY, 0) < 0);
    assert(loadRecord(shared, r) && r.state == 0 && !r.updated_mono_ns);
    munmap(const_cast<TimingRecord *>(shared), 128);
    close(fd);
    puts("PASS: actual POSIX publication lock, <=1 Hz heartbeat, calibration, pause/new "
         "stream/generation/member fences, shutdown zero/unlink");
}
int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "publication")) {
            publication();
            return 0;
        }
        printf("publish=%d every=%u offset=%d\n", publishEnabled(), lockedEvery(), audibleOffset());
        return 0;
    }
    setenv("YENEY_TIMING_PROBE", "1", 1);
    field();
    simulation();
    mapping();
    seqlock();
    polling();
    bracketFreshness();
}
