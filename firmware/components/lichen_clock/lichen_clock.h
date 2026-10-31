#pragma once
// The system clock and whether to trust it (#8). Time comes from SNTP (c6link.c); a warm reset keeps the RTC time.

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include "clock_check.h"

#ifdef __cplusplus
extern "C" {
#endif

// The clock lies in the firmware's plausible window: SNTP (or a restored RTC) has set it. Replaces the 2026-01-01
// constants the status bar, About and MeshCore glue each had.
bool    clock_is_set(void);
int64_t clock_build_epoch(void);

// SNTP offers a time. Sets the system clock and returns CLOCK_OK, or leaves it alone and says why. Records the step.
clock_verdict_t clock_offer_sntp(struct timeval const *tv);

typedef struct {
    bool            synced;       // SNTP set the time since this boot
    int64_t         last_good_s;  // time(NULL) of the last accepted answer, 0 if none
    int64_t         last_good_mono_ms; // uptime at the last accepted answer
    int64_t         step_ms;      // the last accepted answer minus the clock just before it
    int64_t         step_mono_ms; // uptime at that step (the step divided by this is the clock's error since boot)
    clock_verdict_t last_verdict;
    uint32_t        refused;      // answers refused since boot
} clock_sync_info_t;

clock_sync_info_t clock_sync_info(void);

#ifdef __cplusplus
}
#endif
