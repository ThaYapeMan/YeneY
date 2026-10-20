// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstdio>
#include <cstdlib>
namespace bridge {
inline unsigned queueNextDelayMs() {
    unsigned seconds = 20;
    if (const char *text = std::getenv("YENEY_QUEUE_NEXT_DELAY_S")) {
        char *end = nullptr;
        const long value = std::strtol(text, &end, 10);
        if (*text && end && !*end && value >= 1 && value <= 120)
            seconds = unsigned(value);
        else
            std::fprintf(stderr, "Invalid YENEY_QUEUE_NEXT_DELAY_S=%s; using 20\n", text);
    }
    std::printf("YENEY_QUEUE_NEXT_DELAY_S=%u\n", seconds);
    return seconds * 1000;
}
} // namespace bridge
