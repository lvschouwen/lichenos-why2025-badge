#pragma once

#include "ui_perf_stats.h"

#include <stdint.h>

// Frame timing of the display (#93), for the Bench app and "fw ui perf". Both run in LVGL context (the LVGL task, or
// any task holding the LVGL port lock).

// The counters since the last reset
void ui_perf_snapshot(ui_perf_t *out);

// Redraws the whole active screen n times (invalidate + lv_refr_now) and returns the time it took, in microseconds
uint64_t ui_perf_redraw_us(unsigned n);
