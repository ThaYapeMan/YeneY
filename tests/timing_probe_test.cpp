#include "timing_probe.h"
#include <cassert>
#include <cstdio>
#include <random>
using namespace timing_probe;
void simulate(double ppm, unsigned seed) {
    Estimator model;
    Limiter limiter;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> rtt(.006, .018), jitter(-.002, .002), probability(0, 1);
    const double start = 1.237, slope = 1 + ppm / 1e6;
    std::vector<double> error;
    std::deque<double> requests;
    double now = 2, backoff = 0;
    model.seed(std::floor((now - start) * slope), now);
    unsigned lost = 0, count = 0, max1 = 0, max10 = 0;
    while (now < 300) {
        if (now >= model.due && now >= backoff && limiter.take(now)) {
            ++count;
            requests.push_back(now);
            while (!requests.empty() && now - requests.front() >= 10)
                requests.pop_front();
            unsigned recent = 0;
            for (auto t : requests)
                if (now - t < 1)
                    ++recent;
            max1 = std::max(max1, recent);
            max10 = std::max(max10, unsigned(requests.size()));
            double delay = rtt(rng);
            if (probability(rng) < .015) {
                ++lost;
                now += .2;
                backoff = now + 2;
                continue;
            }
            if (probability(rng) < .025)
                delay = .15; // discarded RTT outlier
            double second = std::floor((now + delay / 2 + jitter(rng) - start) * slope);
            model.sample(now, now + delay, second);
            now += delay;
            if (model.edges.size() >= 30)
                error.push_back(std::abs(model.timeAt(second + 1) - (start + (second + 1) / slope)) * 1000);
        } else
            now += .002;
    }
    double p95 = percentile(error, .95), drift = (model.slope - 1) * 1e6;
    printf("simulation seed=%u true_ppm=%.0f fitted_ppm=%.3f edges=%zu truth_error_p95_ms=%.3f lost=%u "
           "rejected=%u requests=%u max_1s=%u max_10s=%u\n",
           seed, ppm, drift, model.edges.size(), p95, lost, model.rejected, count, max1, max10);
    fflush(stdout);
    assert(model.edges.size() > 50 && !error.empty());
    assert(p95 <= 25);
    assert(std::abs(drift - ppm) < 40);
    assert(max1 <= 8 && max10 <= 30);
    model.reset();
    assert(!model.fitted && model.edges.empty() && !model.sampled);
}
int main() {
    for (double ppm : {0., 200., -200.})
        for (unsigned seed : {7u, 19u, 73u})
            simulate(ppm, seed);
    Estimator robust;
    for (unsigned n = 0; n < 40; ++n)
        robust.edges.push_back({10. + n, double(n) + (n == 20 ? .25 : 0), .020});
    robust.fit();
    assert(robust.inliers.size() == 39);
    assert(std::abs(robust.timeAt(45) - 55) < .001);
    puts("PASS: robust regression rejects an isolated 250 ms edge outlier");
    Limiter l;
    for (int i = 0; i < 8; ++i)
        assert(l.take(i * .001));
    assert(!l.take(.009));
    for (int s = 1; s <= 2; ++s)
        for (int i = 0; i < 8; ++i)
            assert(l.take(s * 1.02 + i * .001));
    for (int i = 0; i < 6; ++i)
        assert(l.take(3.06 + i * .001));
    assert(!l.take(3.1));
    assert(l.take(10));
    assert(eligible(true, true, true, true, true));
    for (int gate = 0; gate < 5; ++gate) {
        bool v[5] = {true, true, true, true, true};
        v[gate] = false;
        assert(!eligible(v[0], v[1], v[2], v[3], v[4]));
    }
    puts("PASS: truncated clock, jitter/loss, +/-200 ppm, robust RTT rejection, reset and exact rolling "
         "budgets; all eligibility gates fail closed");
}
