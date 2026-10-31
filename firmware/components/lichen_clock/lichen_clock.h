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
// Set, but only by an app (from mesh contacts or the companion): plausible and never rewound, yet unverified (#83).
// Also a clock a warm reset carried over from such a time (#64): the status bar shows it as ~HH:MM.
bool    clock_is_approximate(void);
int64_t clock_build_epoch(void);

// src offers a time (clock_decide() in clock_check.h has the rules). Sets the system clock and returns CLOCK_OK or
// CLOCK_RECOVERED (see clock_accepted()), or leaves it alone and says why. Thread-safe.
clock_verdict_t clock_propose(clock_src_t src, struct timeval const *tv);

// Set by hand (#64): clock_propose(CLOCK_SRC_MANUAL). force ("fw clock set --force", bench only) judges it as if SNTP
// had not set the clock this boot; the next SNTP answer is then taken in full.
clock_verdict_t clock_set_manual(struct timeval const *tv, bool force);

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
    clock_src_t     was;          // for RTC: who had set it before the reset (clock_src_was()), else NONE (#64)
} clock_sync_info_t;

clock_sync_info_t clock_sync_info(void);

// clock_src_line() for the current source: "SNTP, 3 min ago", "carried over the restart, was SNTP", ...
void clock_source_text(char *out, size_t cap);
// Seconds since the current source set the clock (since boot for RTC)
int64_t clock_source_age_s(void);

// The source record that outlives a warm reset (#64; RTC no-init memory does not survive a software restart on this
// P4, so it is a key in NVS): was is what it held at boot, save(src) writes a new source. Called once at boot by
// clock_store.c; save is called outside the clock's lock, only when the source changes, from the proposer's task.
void clock_set_store(clock_src_t was, void (*save)(clock_src_t src));

// Registers "fw clock" (clock_cmd.c), notes the RTC and esp_timer readings it measures drift against and reads the
// source record (clock_store.c). Call early, after NVS is up.
void clock_console_register(void);

#ifdef __cplusplus
}
#endif
