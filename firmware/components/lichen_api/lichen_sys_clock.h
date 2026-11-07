#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

// The clock's system surface (#244, #216 decision 1): the shell's view of the clock service (Time settings, status bar,
// About, system notices), never lichen_clock.h. System surface: tools/layers.conf forbids lichen_sys_* in L6. The
// types repeat lichen_clock's (the gate cannot include it); lichen_sys_pure_view.c keeps them equal with asserts.
// Every call is safe from any task unless it says otherwise.

typedef enum {
    LICHEN_CLOCK_OK,
    LICHEN_CLOCK_TOO_OLD,
    LICHEN_CLOCK_TOO_NEW,
    LICHEN_CLOCK_BIG_STEP,
    LICHEN_CLOCK_RECOVERED,
    LICHEN_CLOCK_OUTRANKED,
    LICHEN_CLOCK_NOT_FORWARD,
    LICHEN_CLOCK_NO_SOURCE,
} lichen_clock_verdict_t;

typedef enum {
    LICHEN_CLOCK_SRC_NONE,
    LICHEN_CLOCK_SRC_RTC,
    LICHEN_CLOCK_SRC_APP,
    LICHEN_CLOCK_SRC_MANUAL,
    LICHEN_CLOCK_SRC_SNTP,
} lichen_clock_src_t;

// GAP: the time does not exist (spring forward); TWICE: it exists twice (autumn); BAD_DATE: 30 February and the like
typedef enum { LICHEN_CLOCK_LOCAL_OK, LICHEN_CLOCK_LOCAL_GAP, LICHEN_CLOCK_LOCAL_TWICE, LICHEN_CLOCK_LOCAL_BAD_DATE } lichen_clock_local_t;

typedef struct {
    bool                   synced;
    int64_t                last_good_s;
    int64_t                last_good_mono_ms;
    int64_t                step_ms;
    int64_t                step_mono_ms;
    int64_t                step_span_ms;
    lichen_clock_verdict_t last_verdict;
    uint32_t               refused;
    lichen_clock_src_t     source;
    int64_t                source_mono_ms;
    lichen_clock_src_t     was;
} lichen_clock_sync_t;

// The clock lies in the firmware's plausible window (some source set it, or a warm reset kept the RTC). Any task.
bool lichen_sys_clock_is_set(void);
// Set only by an app, or carried over a warm reset from such a time: unverified, the status bar shows ~HH:MM. Any task.
bool lichen_sys_clock_is_approximate(void);
// Commit time of the checkout, the plausible window's anchor. Any task.
int64_t lichen_sys_clock_build_epoch(void);
// t lies in the window around build_epoch. Pure, any task.
bool lichen_sys_clock_in_window(int64_t t, int64_t build_epoch);
// Sets the clock by hand (a proposal from the manual source); the verdict says whether it was taken. Any task but not an
// interrupt; force is bench only (as "fw clock set --force").
lichen_clock_verdict_t lichen_sys_clock_set_manual(struct timeval const *tv, bool force);
// CLOCK_OK and CLOCK_RECOVERED set the clock. Pure, any task.
bool lichen_sys_clock_accepted(lichen_clock_verdict_t v);
// A copy of the SNTP and source state. Any task.
lichen_clock_sync_t lichen_sys_clock_sync_info(void);
// "SNTP, 3 min ago", "carried over the restart, was SNTP", ... Any task.
void lichen_sys_clock_source_text(char *out, size_t cap);
// Seconds since the current source set the clock (since boot for the RTC). Any task.
int64_t lichen_sys_clock_source_age_s(void);
// Why a proposal was refused ("too old", ...). Static text, pure, any task.
char const *lichen_sys_clock_verdict_text(lichen_clock_verdict_t v);

// A local date and time as a person enters it, to UTC in the selected zone. Pure apart from the C library's TZ; read the
// zone on the task that set it (LVGL task for the shell).
int                  lichen_sys_clock_days_in_month(int year, int month);
lichen_clock_local_t lichen_sys_clock_local_to_utc(int year, int month, int day, int hour, int minute, int64_t *utc);
// The hints Set date & time shows, NULL when there is nothing to say. Static text, pure, any task.
char const *lichen_sys_clock_local_hint(lichen_clock_local_t how);
// The new time is more than 5 min behind the clock (MeshCore nodes would ignore our adverts): static hint text or NULL.
// Pure, any task.
char const *lichen_sys_clock_back_hint(int64_t now, int64_t set_to);
