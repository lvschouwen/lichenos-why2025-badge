#pragma once

// The lock-owner check's count mode (#225): every violation counts, and the first few call sites (return address, task
// and LVGL function of the first hit) keep a line each with their own count, so a soak shows where without aborting.
// Pure (host test: ui_call_queue_test.c). Lock free: hits come from any task at any time, the table fills by CAS.

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_LOCK_HIT_SITES 8
#define UI_LOCK_HIT_TASK_LEN 16 // configMAX_TASK_NAME_LEN

typedef struct {
    _Atomic uintptr_t site;  // the caller's return address, 0 = free slot
    _Atomic bool      ready; // task and what are written (set after them)
    char              task[UI_LOCK_HIT_TASK_LEN];
    char const       *what; // the wrapped LVGL function (a string literal)
    _Atomic uint32_t  count;
} ui_lock_hit_site_t;

typedef struct {
    _Atomic uint32_t   total;
    ui_lock_hit_site_t sites[UI_LOCK_HIT_SITES];
} ui_lock_hits_t;

// Counts one hit from site (non-zero). True on the first hit of a site that got its own line: log it then. A hit past a
// full table only counts in total.
bool ui_lock_hits_record(ui_lock_hits_t *h, uintptr_t site, char const *task, char const *what);
// Back to no hits (`fw ui lockcheck reset`). Not atomic against hits in flight: one may land in the old or new table.
void ui_lock_hits_reset(ui_lock_hits_t *h);
