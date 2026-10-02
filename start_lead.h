#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
// Header-only so both C output and C++ encoder use the same validated setting.
static unsigned yeney_lead_value = 1000;
static pthread_once_t yeney_lead_once = PTHREAD_ONCE_INIT;
static void yeney_parse_lead(void) {
        unsigned value;
        const char *text = getenv("YENEY_START_LEAD_MS");
        value = 1000;
        if (text) {
            unsigned parsed = 0; int valid = *text != 0;
            for (const char *p = text; *p; ++p) {
                if (*p < '0' || *p > '9' || parsed > 10000) { valid = 0; break; }
                parsed = parsed * 10 + (unsigned)(*p - '0');
            }
            if (valid && parsed <= 10000) value = parsed;
            else fprintf(stderr, "YENEY_START_LEAD_MS invalid; using 1000 (valid range 0..10000)\n");
        }
        yeney_lead_value = value;
}
static inline unsigned yeney_start_lead_ms(void) {
    pthread_once(&yeney_lead_once, yeney_parse_lead);
    return yeney_lead_value;
}
