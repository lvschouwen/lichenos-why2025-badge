#pragma once

#include <stddef.h>
#include <stdint.h>

// Frame timing of the LVGL display (#93): how long a render takes (RENDER_START..RENDER_READY, every refresh that
// drew something), how long each flush call takes and how long LVGL waits for the previous flush before it reuses a
// draw buffer. Pure accumulation and formatting, host-tested in host_tests/ui_perf_stats_test.c; ui_perf.c feeds it
// from the display events and prints it as "fw ui perf".

typedef struct {
    uint32_t n;
    uint64_t sum_us;
    uint32_t max_us;
} perf_acc_t;

// 1 ms buckets up to PERF_HIST_MAX_MS, the last bucket takes everything slower: enough for a p95 against the standard's
// 16.7 / 33 ms frame limits in 200 bytes
#define PERF_HIST_MAX_MS 99
typedef struct {
    uint16_t bucket[PERF_HIST_MAX_MS + 1];
    uint32_t n;
} perf_hist_t;

typedef struct {
    perf_acc_t render, flush, wait;
    uint32_t   since_ms; // uptime at the last reset
} ui_perf_t;

// The latency side (#205), apart from ui_perf_t so the Bench app's snapshots stay small on its stack
typedef struct {
    perf_acc_t  key; // key press to the end of the next flush
    perf_hist_t render_hist, key_hist;
} ui_perf_lat_t;

void     ui_perf_reset(ui_perf_t *p, uint32_t now_ms);
void     perf_acc_add(perf_acc_t *a, uint32_t us);
uint32_t perf_acc_avg_us(perf_acc_t const *a); // 0 when empty

void     perf_hist_add(perf_hist_t *h, uint32_t us);
// The 95th percentile in whole ms (the upper edge of the bucket holding it); 0 when empty. A value in the last bucket
// reads PERF_HIST_MAX_MS + 1: "at least that".
uint32_t perf_hist_p95_ms(perf_hist_t const *h);

// Second line: "ui perf latency: render p95 14 ms, key-to-flush 5 keys avg 30.1 max 55.0 p95 56 ms, first frame at 1234 ms"
// (first_frame_ms 0 = none seen yet; no keys: "key-to-flush none"). Returns the length.
size_t ui_perf_format_latency(char *out, size_t cap, ui_perf_lat_t const *p, uint32_t first_frame_ms);

// "ui perf: 12 frames in 30.0 s (0.4/s), render avg 41.2 max 97.3 ms, 108 flushes avg 3.1 max 9.0 ms, flush wait avg
// 2.0 max 8.1 ms"; "ui perf: no frames in 30.0 s" when nothing was drawn. Returns the length.
size_t ui_perf_format(char *out, size_t cap, ui_perf_t const *p, uint32_t now_ms);
