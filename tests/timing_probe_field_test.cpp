#include "timing_probe.h"
#include <cassert>
#include <cstdio>
#include <random>
using namespace timing_probe;
void field(double ppm, unsigned seed) {
    Estimator model;
    Limiter limiter;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> probability(0, 1), rtt(.004, .020), delay(.100, .300);
    const double start = 1.237, rate = 1 + ppm / 1e6;
    std::vector<double> ticks(605), errors;
    std::vector<bool> delayed(605);
    for (unsigned i = 0; i < ticks.size(); ++i) {
        delayed[i] = probability(rng) < .10;
        ticks[i] = start + i / rate + (delayed[i] ? delay(rng) : 0);
    }
    unsigned flagged = 0, lost = 0, slow = 0, reports = 0, max1 = 0, max10 = 0;
    double worstDrift = 0, now = 2, backoff = 0;
    std::deque<double> requests;
    model.seed(0, now);
    while (now < 600) {
        assert(!model.stale(now, 2, 60));
        if (now < model.due || now < backoff || !limiter.take(now)) {
            now += .002;
            continue;
        }
        requests.push_back(now);
        while (now - requests.front() >= 10)
            requests.pop_front();
        unsigned recent = 0;
        for (double t : requests)
            recent += now - t < 1;
        max1 = std::max(max1, recent);
        max10 = std::max(max10, unsigned(requests.size()));
        double duration = rtt(rng);
        if (probability(rng) < .015) {
            ++lost;
            now += .2;
            backoff = now + 2;
            continue;
        }
        if (probability(rng) < .015) {
            duration = .060;
            ++slow;
        }
        const double observed = now + duration / 2;
        const auto n =
            unsigned(std::upper_bound(ticks.begin(), ticks.end(), observed) - ticks.begin() - 1);
        model.sample(now, now + duration, n);
        if (model.decision.observed && delayed[n] && !model.decision.inlier)
            ++flagged;
        now += duration;
        if (now > 120 && model.fitted)
            errors.push_back(std::abs(model.timeAt(n + 1) - (start + (n + 1) / rate)) * 1000);
        if (model.driftReported) {
            assert(model.span >= 120 && model.driftSigma <= 15);
            ++reports;
            worstDrift = std::max(worstDrift, std::abs((model.slope - 1) * 1e6 - ppm));
        }
    }
    const double p95 = percentile(errors, .95);
    printf("field seed=%u true_ppm=%.0f fitted_ppm=%.3f inlier_edges=%llu recent_inliers=%zu "
           "truth_error_p95_ms=%.3f worst_reported_drift_error_ppm=%.3f reports=%u "
           "delayed_outliers=%u missed=%u lost=%u slow=%u max_1s=%u max_10s=%u\n",
           seed, ppm, (model.slope - 1) * 1e6, (unsigned long long)model.usableEdges,
           model.inliers.size(), p95, worstDrift, reports, flagged, model.missed, lost, slow, max1,
           max10);
    fflush(stdout);
    assert(model.usableEdges >= 300);
    assert(p95 <= 25);
    assert(worstDrift <= 30);
    assert(ppm == 0 || reports > 0);
    assert(flagged > 10);
    assert(max1 <= 8 && max10 <= 30);
}
int main() {
    for (double ppm : {0., 200., -200.})
        for (unsigned seed : {7u, 19u, 73u})
            field(ppm, seed);
    Limiter dispatch;
    for (unsigned n = 0; n < 30; ++n) {
        assert(dispatch.take(n * .34));
        dispatch.sent(n * .34 + .005);
    }
    assert(!dispatch.take(10));
    assert(dispatch.take(10.006));
    puts(
        "PASS: send timestamp reservation preserves the exact rolling budget at the 10 s boundary");
    puts("PASS: 600 s field clocks: delayed/missing ticks, rare slow RTT, robust phase/drift and "
         "exact budgets");
}
