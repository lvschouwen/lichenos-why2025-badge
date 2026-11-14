#pragma once
// The bookkeeping behind lichen_timer (#231): which context owns which booked timer, under a handle that is never
// reused (slot plus generation), so a stale handle (a timer already stopped, or dropped with its app's stop) matches
// nothing. One small record per timer. Pure (no LVGL), host-tested in host_tests/timer_book_test.c; lichen_ui_cb.c
// is the LVGL glue and keeps the book on the LVGL task (under its lock), so these calls take no lock themselves.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIMER_BOOK_SLOTS     16 // booked timers that exist at the same time
#define TIMER_BOOK_SLOT_BITS 5u // a handle's low bits: the slot + 1 (0 is never a handle)

typedef struct {
    uint32_t handle; // 0 = free slot
    uint32_t ctx;    // the owner's context (lichen_ctx_t, #230)
    void (*cb)(void *arg);
    void *arg;
} timer_slot_t;

typedef struct {
    timer_slot_t slot[TIMER_BOOK_SLOTS];
    uint32_t     gen; // the last generation handed out
} timer_book_t;

// A handle for a new booked timer of ctx; 0 when the book is full, ctx is 0 or cb is NULL
uint32_t timer_book_add(timer_book_t *b, uint32_t ctx, void (*cb)(void *arg), void *arg);
// The slot of a live handle, NULL for 0, a stale or a made-up one
timer_slot_t const *timer_book_find(timer_book_t const *b, uint32_t handle);
// Takes a live handle out; false (nothing changes) for any other
bool timer_book_remove(timer_book_t *b, uint32_t handle);
// ctx's booked timers
size_t timer_book_count(timer_book_t const *b, uint32_t ctx);
// The handle of ctx's first booked timer at or after slot *from (0 for the first call), 0 when there is none left;
// *from moves past it, so a loop that removes what it gets still visits every one
uint32_t timer_book_next(timer_book_t const *b, uint32_t ctx, size_t *from);

#ifdef __cplusplus
}
#endif
