// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace timing_probe {
inline int integerSetting(const char *key, int fallback, int low, int high) {
    const char *s = getenv(key);
    if (!s)
        return fallback;
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    const char *digits = s + (*s == '-' ? 1 : 0);
    bool valid = *digits;
    for (auto p = digits; *p; ++p)
        if (*p < '0' || *p > '9')
            valid = false;
    if (errno || !valid || *end || n < low || n > high) {
        printf("yeney: setting key=%s invalid=%s fallback=%d\n", key, s, fallback);
        return fallback;
    }
    return int(n);
}
inline bool lmsPositionFromModel() {
    static const bool on = [] {
        const char *s = getenv("YENEY_LMS_POSITION_FROM_MODEL");
        if (s && strcmp(s, "0") && strcmp(s, "1"))
            printf("yeney: setting key=YENEY_LMS_POSITION_FROM_MODEL invalid=%s fallback=1\n", s);
        return !s || strcmp(s, "0");
    }();
    return on;
}
inline bool publishEnabled() {
    if (!enabled())
        return false;
    static const bool on = [] {
        const char *s = getenv("YENEY_TIMING_PUBLISH");
        if (s && strcmp(s, "0") && strcmp(s, "1"))
            printf("yeney: setting key=YENEY_TIMING_PUBLISH invalid=%s fallback=0\n", s);
        return s && !strcmp(s, "1");
    }();
    return on;
}
inline unsigned lockedEvery() {
    if (!enabled())
        return 5;
    static unsigned n = integerSetting("YENEY_TIMING_LOCKED_EVERY_S", 5, 1, 60);
    return n;
}
inline int audibleOffset() {
    if (!publishEnabled())
        return 0;
    static int n = integerSetting("YENEY_AUDIBLE_OFFSET_MS", 0, -500, 500);
    return n;
}
} // namespace timing_probe
