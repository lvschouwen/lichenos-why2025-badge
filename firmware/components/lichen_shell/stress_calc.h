#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The stress run's rules and lines (#278). Pure, host-tested in host_tests/stress_calc_test.c; stress_workers.c runs
// the load and feeds these. Every line here is the text after "stress: " (ESP_LOG adds "I (<ms>) stress: "), one
// physical line, parsable by grep. STRESS_LINE_MAX keeps a line and that prefix under the log ring's 200-character cut
// (log_ring.h) with every figure at the widest the badge can show (internal RAM under 1 MB, PSRAM 32 MB).

#define STRESS_MIN_DEFAULT 10
#define STRESS_MIN_MAX     30
#define STRESS_LINE_MAX    176

// Stop limits (#278 plan): DMA free under the floor (outside a DMA gate round, which holds 8 KB of it for seconds), a
// task stack under the floor that fell during the run, and the latency probe's worst lateness over the limit. A stack
// that was already under the floor at the start fails only when it falls more than STRESS_STACK_MARGIN further.
#define STRESS_LIMIT_DMA    (16u * 1024)
#define STRESS_LIMIT_STACK  256u
#define STRESS_STACK_MARGIN 32u
#define STRESS_LIMIT_LAT_MS 250u

typedef enum { STRESS_CMD_START, STRESS_CMD_STOP, STRESS_CMD_STATUS, STRESS_CMD_BAD } stress_cmd_t;

// "fw stress [minutes|stop|status]": "" is START with STRESS_MIN_DEFAULT, 1..30 START with that, anything else BAD
stress_cmd_t stress_parse(char const *args, unsigned *minutes);

// Why a run ended. The failures first (stress_failed), the judged ones in the order stress_judge checks them; then the
// stops that are not failures.
typedef enum {
    STRESS_NONE,
    STRESS_MISMATCH,  // a file or a PSRAM copy read back different
    STRESS_DMA_BAD,   // the DMA gate saw bad bytes or a DMA error
    STRESS_HEAP,      // a heap integrity check failed (CORRUPT HEAP)
    STRESS_ALLOC,     // a worker could not get its buffers
    STRESS_LOCKCHECK, // `fw ui lockcheck` counted a hit during the run
    STRESS_LATENCY,   // the 10 ms probe woke later than STRESS_LIMIT_LAT_MS
    STRESS_DMA_FLOOR, // DMA-capable RAM under STRESS_LIMIT_DMA
    STRESS_STACK,     // a task stack fell under STRESS_LIMIT_STACK
    STRESS_HUNG,      // workers still running a minute after the stop (not judged: the monitor sets it)
    STRESS_FAILURES,  // (count; not a reason)
    STRESS_ESC = STRESS_FAILURES, // Esc held 3 s: the key lock (#190) ended
    STRESS_STOP_CMD,  // fw stress stop
    STRESS_POWEROFF,
    STRESS_UPDATE,    // an update started
    STRESS_TRIAL,     // the 120 MHz PSRAM trial started: its DMA gate must have the engine to itself
    STRESS_HOT,       // a thermal warning
    STRESS_C6,        // the C6 link is not up
    STRESS_REASONS,
} stress_reason_t;

char const *stress_reason_name(stress_reason_t r); // "mismatch", "dma_bad", ... "esc", "stop", "poweroff", "update"
bool        stress_failed(stress_reason_t r);

typedef struct {
    uint32_t dma_free, dma_min, dma_largest;
    uint32_t int_free, int_min, int_largest;
    uint32_t psram_free;
    uint32_t pool_peak, pool_drop;
    uint32_t lat_ms; // the probe's worst lateness in the sample's window
} stress_sample_t;

// What one 10 s check sees: counts are totals since the run started
typedef struct {
    stress_sample_t s;
    uint32_t        mismatches, dma_bad, alloc_fails, lock_hits;
    bool            heap_ok;
    bool            in_dma_round; // a DMA gate round ran while the sample was taken: the DMA floor is not judged
    uint32_t        stack_low; // the lowest stack free of the tasks stress_stack_fell_low names (UINT32_MAX: none)
} stress_obs_t;

// The first failure the observation shows, STRESS_NONE when it is clean
stress_reason_t stress_judge(stress_obs_t const *o);
// A task's stack counts for STRESS_STACK when free_now is under the floor and the run took it there: it started at or
// over the floor (UINT32_MAX for a task that started during the run), or it fell more than STRESS_STACK_MARGIN below
// an already low start (an IDF task that sits low from boot is reported in the stack lines, not failed)
bool stress_stack_fell_low(uint32_t free_now, uint32_t free_at_start);
// The lockcheck hits since the run started: now - base, or now when the counts were reset meanwhile (now < base)
uint32_t stress_since(uint32_t now, uint32_t base);

// What may end a run from outside the rules, checked every second; the first that holds, in this order
typedef struct {
    stress_reason_t asked; // stress_stop's reason, STRESS_NONE when none
    bool            poweroff, update, trial, hot, c6_trouble;
} stress_outside_t;
stress_reason_t stress_outside(stress_outside_t const *in);

// The run's record: the first stop wins, later ones are ignored; lo holds the lowest free figures and the highest pool,
// drop and latency figures seen
typedef struct {
    stress_reason_t reason;
    uint32_t        stop_s;
    bool            have;
    stress_sample_t lo;
} stress_run_t;

void stress_run_reset(stress_run_t *run);
void stress_track(stress_run_t *run, stress_sample_t const *s);
// True when this stop was the first (log the STOP line then)
bool stress_latch(stress_run_t *run, stress_reason_t r, uint32_t t_s);

// What the refusal check sees at start
typedef struct {
    bool     running, update, poweroff, c6_trouble, hot, psram_trial;
    uint32_t dma_free, psram_free;
} stress_gate_in_t;

#define STRESS_START_DMA   (48u * 1024)        // the worker stacks (22 KB) and the DMA gate's 8 KB, with room
#define STRESS_START_PSRAM (8u * 1024 * 1024)  // the gate's 3 MB, the copies' 2 MB and the SD buffers, with room

// Why a run may not start (a short phrase for the console and the screen), NULL when it may
char const *stress_refusal(stress_gate_in_t const *in);

// The run's counts: the errors first (the "errors" line, dma_busy is a skip, not an error), then the work
typedef enum {
    STRESS_C_DMA_BAD, STRESS_C_DMA_BUSY, STRESS_C_MISMATCH, STRESS_C_ALLOC, STRESS_C_HEAP, STRESS_C_LOCK,
    STRESS_C_SD_IO, STRESS_C_FS_IO,
    STRESS_C_WORK, // the first work count
    STRESS_C_DMA = STRESS_C_WORK, STRESS_C_COPIES, STRESS_C_SD, STRESS_C_SD_KB, STRESS_C_FS, STRESS_C_FRAMES,
    STRESS_C_COUNT,
} stress_count_t;

typedef struct {
    uint32_t n[STRESS_C_COUNT];
} stress_counts_t;

// The errors, dma_busy left out
uint32_t stress_errors(stress_counts_t const *c);

// The lines (snprintf rules: the length the whole line needs, out always terminated when cap > 0)
// "start 10 min workers dma,sd,fs,ui,lat"; without a card "start 10 min workers dma,fs,ui,lat sd skipped: no card"
size_t stress_fmt_start(char *out, size_t cap, unsigned minutes, bool sd, bool fs);
// "t=10s dma free=.. min=.. largest=.. int free=.. min=.. largest=.. psram=.. pool peak=.. drop=.. lat=..ms ok" (the
// reason name instead of ok once one is latched)
size_t stress_fmt_sample(char *out, size_t cap, uint32_t t_s, stress_sample_t const *s, stress_reason_t r);
// "min dma free=.. min=.. largest=.. int free=.. min=.. largest=.. psram=.. pool peak=.. drop=.. lat=..ms"
size_t stress_fmt_min(char *out, size_t cap, stress_sample_t const *lo);
size_t stress_fmt_stop(char *out, size_t cap, stress_reason_t r, uint32_t t_s); // "STOP latency at 42s"
size_t stress_fmt_done(char *out, size_t cap, uint32_t t_s, bool pass);         // "done 600s PASS"
size_t stress_fmt_stack(char *out, size_t cap, char const *task, uint32_t free, uint32_t size); // "stack ipc0 812/1024", size 0: "?"
// "errors dma_bad=0 dma_busy=0 mismatch=0 alloc=0 heap=0 lock=0 sd_io=0 fs_io=0" (work false) or
// "work dma=30 copies=30 sd=100 sd_kb=6400 fs=60 frames=6000" (work true)
size_t stress_fmt_counts(char *out, size_t cap, stress_counts_t const *c, bool work);

// The run's screen (app_stress.c), pure so it is host-tested too
// "03:20 / 10:00" (minutes past 99 are not reached: a run is at most 30 min)
size_t stress_fmt_clock(char *out, size_t cap, uint32_t t_s, uint32_t total_s);
// The workers, as bits: the ones that failed (their own data went wrong, or for UI a lockcheck hit and for the latency
// probe a STRESS_LATENCY stop) and the ones skipped (only SD and storage can be)
#define STRESS_W_DMA 1u
#define STRESS_W_SD  2u
#define STRESS_W_FS  4u
#define STRESS_W_UI  8u
#define STRESS_W_LAT 16u
// One line per worker, "DMA: ok, 12 rounds\nSD: skipped, no card\nStorage: FAIL, 3 files\nUI: ok, 600 frames\n
// Latency: ok, worst 4 ms"
size_t stress_fmt_workers(char *out, size_t cap, stress_counts_t const *c, unsigned failed, unsigned skipped, uint32_t lat_ms);
