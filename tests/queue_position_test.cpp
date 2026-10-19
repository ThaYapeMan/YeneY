// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "position_state.h"
#include <cassert>
#include <iostream>
int main() {
    ConnectionPosition clock;
    const double prior = 1.0002;
    clock.item(1, 0, 44100, 100000);
    assert(clock.smoothFrames(44100, 100000, false, 0, prior) == 0);
    assert(clock.smoothFrames(44100, 100100, false, 0, prior) >= 4410);
    clock.itemPoll(5000, 105000);
    auto old = clock.smoothFrames(44100, 105100, false, 0, prior);
    assert(old >= 5 * 44100);
    // Native rate changes carry the qualified device rate, not the old phase.
    clock.item(2, 0, 48000, 106000);
    assert(clock.smoothFrames(48000, 106000, false, 0, prior) == 0);
    auto frame = clock.smoothFrames(48000, 106500, false, 0, prior);
    assert(frame == uint64_t(24000 * prior));
    clock.itemPoll(0, 106600); // leased zero does not freeze or reanchor
    assert(clock.smoothFrames(48000, 106600, false, 0, prior) > frame);
    clock.item(3, 0, 48000, 200000, frame); // resume at frozen within-item phase
    assert(clock.smoothFrames(48000, 200100, false, 0, prior) > frame);
    std::cout
        << "PASS queue position model: confirmed zero moves immediately, per-item phase reset, native-rate "
           "prior preserved, leased zero does not freeze, resume excludes paused wall time\n";
}
