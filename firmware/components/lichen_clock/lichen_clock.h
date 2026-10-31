#pragma once
// The system clock and whether to trust it (#8). The OS owns it (#83): every source goes through clock_propose(), the
// only settimeofday() caller. Time comes from SNTP (c6link.c), an app (MeshCore's contacts or companion), by hand
// (#64); a warm reset keeps the RTC time.

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include "clock_check.h"

#ifdef __cplusplus
extern "C" {
#endif

// The clock lies in the firmware's plausible window: some source (or a restored RTC) has set it. Replaces the
// 2026-01-01 constants the status bar, About and MeshCore glue each had.
bool    clock_is_set(void);
// Set, but only by an app (from mesh contacts or the companion): plausible and never rewound, yet unverified (#83)
bool    clock_is_approximate(void);
int64_t clock_build_epoch(void);

// src offers a time (clock_decide() in clock_check.h has the rules). Sets the system clock and returns CLOCK_OK or
// CLOCK_RECOVERED (see clock_accepted()), or leaves it alone and says why. Thread-safe.
clock_verdict_t clock_propose(clock_src_t src, struct timeval const *tv);

// SNTP offers a time: clock_propose(CLOCK_SRC_SNTP), and records the step.
clock_verdict_t clock_offer_sntp(struct timeval const *tv);

typedef struct {
    bool            synced;       // SNTP set the time since this boot
    int64_t         last_good_s;  // time(NULL) of the last accepted answer, 0 if none
    int64_t         last_good_mono_ms; // uptime at the last accepted answer
    int64_t         step_ms;      // the last accepted answer minus the clock just before it
    int64_t         step_mono_ms; // uptime at that step (the step divided by this is the clock's error since boot)
    clock_verdict_t last_verdict;
    uint32_t        refused;      // answers refused since boot
    clock_src_t     source;       // who set the clock (#83); RTC when a warm reset carried a plausible time over
    int64_t         source_mono_ms; // uptime when that source set it (0 for RTC: at boot)
} clock_sync_info_t;

clock_sync_info_t clock_sync_info(void);

// clock_src_line() for the current source: "SNTP, 3 min ago", "from mesh contacts, unverified", ...
void clock_source_text(char *out, size_t cap);

// Registers "fw clock" (clock_cmd.c) and notes the RTC and esp_timer readings it measures drift against. Call early.
void clock_console_register(void);

#ifdef __cplusplus
}
#endif
