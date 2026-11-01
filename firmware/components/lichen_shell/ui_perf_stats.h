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

typedef struct {
    perf_acc_t render, flush, wait;
    uint32_t   since_ms; // uptime at the last reset
} ui_perf_t;

void     ui_perf_reset(ui_perf_t *p, uint32_t now_ms);
void     perf_acc_add(perf_acc_t *a, uint32_t us);
uint32_t perf_acc_avg_us(perf_acc_t const *a); // 0 when empty

// "ui perf: 12 frames in 30.0 s (0.4/s), render avg 41.2 max 97.3 ms, 108 flushes avg 3.1 max 9.0 ms, flush wait avg
// 2.0 max 8.1 ms"; "ui perf: no frames in 30.0 s" when nothing was drawn. Returns the length.
size_t ui_perf_format(char *out, size_t cap, ui_perf_t const *p, uint32_t now_ms);
