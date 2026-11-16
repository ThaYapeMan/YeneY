// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdint>
namespace bridge {
// A new failed connection attempt can remind the journal once per ten minutes.
class PositionFailureLog {
    uint64_t seen = 0;
    double last = -600;
public:
    bool due(double now, uint64_t attempts) {
        if (attempts == seen) return false;
        seen = attempts;
        if (!attempts || now - last < 600) return false;
        last = now;
        return true;
    }
};
}
