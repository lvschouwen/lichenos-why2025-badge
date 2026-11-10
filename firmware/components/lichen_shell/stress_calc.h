#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Stress app's rules and lines (#278). Pure, host-tested in host_tests/stress_calc_test.c; stress_workers.c runs
// the load and feeds these. Every line here is the text after "stress: " (ESP_LOG adds "I (<ms>) stress: "), one
// physical line, parsable by grep. STRESS_LINE_MAX keeps a line and that prefix under the log ring's 200-character cut
// (log_ring.h) with every figure at the widest the badge can show (internal RAM under 1 MB, PSRAM 32 MB).

#define STRESS_MIN_DEFAULT 10
#define STRESS_MIN_MAX     30
#define STRESS_LINE_MAX    176

// Stop limits (#278 plan): DMA free under the floor, a task stack under the floor that fell during the run, and the
// latency probe's worst lateness over the limit
#define STRESS_LIMIT_DMA   (16u * 1024)
#define STRESS_LIMIT_STACK 256u
#define STRESS_LIMIT_LAT_MS 250u

typedef enum { STRESS_CMD_START, STRESS_CMD_STOP, STRESS_CMD_STATUS, STRESS_CMD_BAD } stress_cmd_t;

// "fw stress [minutes|stop|status]": "" is START with STRESS_MIN_DEFAULT, 1..30 START with that, anything else BAD
stress_cmd_t stress_parse(char const *args, unsigned *minutes);

// Why a run ended. The failures first (stress_failed), in the order stress_judge checks them; then the stops that are
// not failures.
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
    STRESS_FAILURES,  // (count; not a reason)
    STRESS_ESC = STRESS_FAILURES, // Esc on the Stress screen
    STRESS_STOP_CMD,  // fw stress stop
    STRESS_POWEROFF,
    STRESS_UPDATE,    // an update started
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
    uint32_t        stack_low; // the lowest stack free of the tasks whose high-water fell during the run (UINT32_MAX: none)
} stress_obs_t;

// The first failure the observation shows, STRESS_NONE when it is clean
stress_reason_t stress_judge(stress_obs_t const *o);
// A task's stack counts for STRESS_STACK only when it fell during the run (an IDF task that sits low from boot is
// reported in the stack lines, not failed): free_now under the floor and under free_at_start
bool stress_stack_fell_low(uint32_t free_now, uint32_t free_at_start);

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

// The work and error counts of a run (the "work" and "errors" lines)
typedef struct {
    uint32_t dma_rounds, dma_busy, dma_bad, copies, mismatches, alloc_fails, heap_bad, lock_hits;
    uint32_t sd_cycles, sd_kb, sd_io, fs_cycles, fs_io, frames;
} stress_counts_t;

// The lines (snprintf rules: the length the whole line needs, out always terminated when cap > 0)
// "start 10 min workers dma,sd,fs,ui,lat"; without a card "start 10 min workers dma,fs,ui,lat sd skipped: no card"
size_t stress_fmt_start(char *out, size_t cap, unsigned minutes, bool sd, bool fs);
// "t=10s dma free=.. min=.. largest=.. int free=.. min=.. largest=.. psram=.. pool peak=.. drop=.. lat=..ms ok" (the
// reason name instead of ok once one is latched)
size_t stress_fmt_sample(char *out, size_t cap, uint32_t t_s, stress_sample_t const *s, stress_reason_t r);
size_t stress_fmt_stop(char *out, size_t cap, stress_reason_t r, uint32_t t_s); // "STOP latency at 42s"
size_t stress_fmt_done(char *out, size_t cap, uint32_t t_s, bool pass);         // "done 600s PASS"
// "min dma free=.. min=.. largest=.. int free=.. min=.. largest=.. psram=.. pool peak=.. drop=.. lat=..ms"
size_t stress_fmt_min(char *out, size_t cap, stress_sample_t const *lo);
size_t stress_fmt_stack(char *out, size_t cap, char const *task, uint32_t free, uint32_t size); // "stack ipc0 812/1024", size 0: "?"
size_t stress_fmt_errors(char *out, size_t cap, stress_counts_t const *c);
size_t stress_fmt_work(char *out, size_t cap, stress_counts_t const *c);
