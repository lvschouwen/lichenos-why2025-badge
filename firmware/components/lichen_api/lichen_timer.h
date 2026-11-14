#pragma once
// Booked timers for apps (#231): a periodic callback on the LVGL task that runs as the app (#349) and is booked under
// its context, so the app's stop(reason) takes every one of them away with it and "fw app life" can count them. The
// app holds a handle, not an LVGL timer: a handle that was stopped, or dropped with the app's stop, matches nothing,
// so stopping it again is harmless. For a timer that belongs to one screen, lichen_ui_timer_create (lichen_ui.h) is
// still the simple choice; a booked timer outlives screens until it is stopped.

#include "lichen_app.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t lichen_timer_t;
#define LICHEN_TIMER_NONE ((lichen_timer_t)0)

// Runs cb(arg) every period_ms (at least 1) on the LVGL task, inside ctx's context, until lichen_timer_stop or ctx's
// app is stopped. ctx is the app's own (lichen_register_app's); a stale or made-up one is refused (logged, counted in
// "fw app ctx"). LICHEN_TIMER_NONE when refused, when the book is full (TIMER_BOOK_SLOTS booked timers in all) or
// without memory. cb may stop its own timer. LVGL task only.
lichen_timer_t lichen_timer_start(lichen_ctx_t ctx, uint32_t period_ms, void (*cb)(void *arg), void *arg);
// Stops and deletes ctx's timer t; false for a handle that is not ctx's live one (already stopped, dropped with the
// app's stop, made up), which changes nothing. LVGL task only.
bool lichen_timer_stop(lichen_ctx_t ctx, lichen_timer_t t);

#ifdef __cplusplus
}
#endif
