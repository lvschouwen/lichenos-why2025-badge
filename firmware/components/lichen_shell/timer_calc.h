#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Timer app's pure logic (#152): a countdown and a stopwatch as small state machines on a monotonic millisecond
// clock the caller passes in (so they survive the screen going away and are testable), the text for their readouts,
// the "5:30" duration input, and a world clock that reads POSIX TZ strings itself (setenv("TZ") is process-wide, and
// the other zones must not disturb the badge's own). Host-tested in host_tests/timer_calc_test.c.

// ---- countdown ----
typedef enum { TCD_IDLE, TCD_RUNNING, TCD_PAUSED, TCD_DONE } tcd_state_t;
typedef struct {
    tcd_state_t state;
    int64_t     set_ms;  // what the countdown starts from
    int64_t     left_ms; // IDLE/PAUSED: what is left; RUNNING: unused
    int64_t     end_ms;  // RUNNING: when it reaches zero
} tcd_t;

#define TCD_MAX_MS (99LL * 3600 * 1000 + 59 * 60 * 1000 + 59 * 1000) // 99:59:59

void    tcd_init(tcd_t *t);
// Sets the duration and resets to idle; ignored (false) while it runs or is paused (reset first), or past TCD_MAX_MS
bool    tcd_set(tcd_t *t, int64_t ms);
// Starts from idle or paused, or again from the full duration after it finished; false when no duration is set
bool    tcd_start(tcd_t *t, int64_t now_ms);
void    tcd_pause(tcd_t *t, int64_t now_ms);
void    tcd_reset(tcd_t *t); // back to idle with the full duration
int64_t tcd_left(tcd_t const *t, int64_t now_ms); // ms still to go, 0 once done
// True exactly once, when a running countdown has reached zero (state is DONE from then on)
bool    tcd_poll(tcd_t *t, int64_t now_ms);

// ---- stopwatch ----
#define SW_MAX_LAPS 99
typedef struct {
    bool    running;
    int64_t start_ms;
    int64_t acc_ms; // elapsed before the current run
    int64_t lap_total[SW_MAX_LAPS]; // elapsed at each lap, in order
    size_t  laps;
} sw_t;

void    sw_init(sw_t *s);
void    sw_start(sw_t *s, int64_t now_ms);
void    sw_stop(sw_t *s, int64_t now_ms);
void    sw_reset(sw_t *s);
int64_t sw_elapsed(sw_t const *s, int64_t now_ms);
// Marks a lap while running; false when stopped or all SW_MAX_LAPS are used
bool    sw_lap(sw_t *s, int64_t now_ms);
// Lap number n (1-based): its split (since the lap before) and the total at it; false past the last
bool    sw_lap_get(sw_t const *s, size_t n, int64_t *split_ms, int64_t *total_ms);

// ---- text ----
// Countdown readout, rounded up to the second so "0:00" only shows when it is over: "5:00", "0:07", "1:02:03"
size_t tfmt_countdown(char *out, size_t n, int64_t ms);
// Stopwatch readout in tenths, rounded down: "00:12.3", "1:02:03.4"
size_t tfmt_stopwatch(char *out, size_t n, int64_t ms);
// A duration in words for a notice: "5 min", "1 min 30 s", "2 h 5 min", "45 s"
size_t tfmt_words(char *out, size_t n, int64_t ms);
// "5" = 5 minutes, "5:30" = 5 min 30 s, "1:00:00" = 1 h. Seconds and minutes after the first colon are 0..59. false for
// anything else, zero or more than 99:59:59.
bool   tdur_parse(char const *text, int64_t *ms);

// ---- world clock ----
typedef struct {
    int  wday; // 0 = Sunday
    int  hour;
    int  min;
    int  offset_s; // seconds east of UTC
    bool dst;
} wc_local_t;
// The local time in the zone of a POSIX TZ string ("CET-1CEST,M3.5.0,M10.5.0/3") at utc (seconds since 1970). false
// when the string is not one this understands (names of 3+ letters, offsets, Mm.w.d rules).
bool wc_local(char const *posix, int64_t utc, wc_local_t *out);
// "Tue 14:32"
size_t wc_fmt(char *out, size_t n, wc_local_t const *l);
