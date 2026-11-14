// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_position.h"
#include <cassert>
#include <iostream>
int main() {
    bridge::PositionFit fit;
    assert(!fit.position(0));
    assert(fit.add({"one", "PLAYING", 1000, 1, 10}));
    assert(std::abs(fit.position(1.1)->positionMs - 1100) < .001);
    for (int i = 1; i <= 20; ++i)
        assert(fit.add({"one", "PLAYING", 1000.0 + i * 200, 1.0 + i * .2, i % 5 ? 10.0 : 50.0}));
    const auto before = fit.position(5.1)->positionMs;
    assert(!fit.add({"one", "PLAYING", 9000, 5.1, 201}));
    assert(std::abs(fit.position(5.1)->positionMs - before) < 5);
    assert(!fit.add({"one", "PLAYING", 5600, 5.2, 10}));
    assert(fit.add({"one", "PLAYING", 5800, 5.4, 10}));
    assert(std::abs(fit.position(5.4)->positionMs - 5800) < .001);
    assert(fit.add({"two", "PLAYING", 0, 5.5, 10}));
    assert(fit.position(5.5)->positionMs == 0);
    assert(fit.add({"two", "PAUSED", 100, 5.6, 10}));
    assert(fit.position(10)->positionMs == 100);
    std::cout << "PASS T8 fit: RTT >200 ms discarded; outlier movement <5 ms; confirmed 400 ms seek reset in 200 ms; first sample, item and pause reset\n";
}
