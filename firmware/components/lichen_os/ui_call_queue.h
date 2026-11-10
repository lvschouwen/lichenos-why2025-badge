#pragma once

// The UI service's call queue (#225): calls handed to the LVGL task wait here in order, and one drain (an LVGL async
// call) runs a batch of them. Pure (host test: ui_call_queue_test.c). Not thread safe by itself: every function is
// called with the LVGL lock held, which is what orders the producers (any task) against the drain (the LVGL task).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_CALL_QUEUE_CAP 16 // a burst larger than this falls back to one LVGL async call per call (lichen_ui_call.c)

typedef void (*ui_call_fn_t)(void *);

typedef struct {
    ui_call_fn_t fn;
    void        *arg;
} ui_call_t;

typedef struct {
    ui_call_t slot[UI_CALL_QUEUE_CAP];
    uint8_t   head;      // oldest call
    uint8_t   count;     // calls waiting
    bool      scheduled; // a drain is on its way: it has not started yet, so it will see every call queued now
} ui_call_queue_t;

typedef enum {
    UI_CALL_QUEUED,         // a drain was already on its way, nothing more to do
    UI_CALL_QUEUED_SCHEDULE, // queued, and the caller must schedule the drain now (or ui_call_queue_unpush on failure)
    UI_CALL_FULL,           // not queued: the caller hands fn to the LVGL task another way
} ui_call_push_t;

ui_call_push_t ui_call_queue_push(ui_call_queue_t *q, ui_call_fn_t fn, void *arg);
// Takes back the call the last push queued, when scheduling its drain failed; the queue is as before that push.
void ui_call_queue_unpush(ui_call_queue_t *q);
// The drain starts: from here a push schedules a new drain. Returns how many calls this drain runs (those queued before
// it started); calls queued while it runs are the next drain's, so a call that queues itself never runs inline.
size_t ui_call_queue_begin_drain(ui_call_queue_t *q);
// The oldest call, taken off the queue before it runs (so it may queue again). False when the queue is empty. A
// cancelled call pops with fn NULL: the drain skips it (its slot keeps the batch's count right).
bool ui_call_queue_pop(ui_call_queue_t *q, ui_call_t *out);
// Cancels every queued fn(arg) that has not started (as lv_async_call_cancel does for LVGL's own): the memory arg
// points to is about to go. Returns how many were cancelled.
size_t ui_call_queue_cancel(ui_call_queue_t *q, ui_call_fn_t fn, void *arg);

// An LVGL async call as the queue sees it (lv_async_call in lichen_ui_call.c, a fake in the host test): true when
// fn(arg) will run on the LVGL task later.
typedef bool (*ui_call_async_t)(ui_call_fn_t fn, void *arg);

// The hand-over lichen_sys_ui_call makes, lock held: queues fn(arg) and, for the first call since the last drain
// started, schedules the drain as schedule(ui_call_queue_drain, q); when that fails the call is taken back. A full
// queue hands fn(arg) to fallback instead (one async call of its own, which may overtake the queue). True when fn
// will run; false leaves arg with the caller.
bool ui_call_queue_hand_over(ui_call_queue_t *q, ui_call_fn_t fn, void *arg, ui_call_async_t schedule,
                             ui_call_async_t fallback);
// lichen_sys_ui_cancel, lock held: cancels fn(arg) in the queue and, through cancel_fallback (lv_async_call_cancel in
// lichen_ui_call.c, a fake in the host test), every one a full queue handed to the fallback.
void ui_call_queue_cancel_all(ui_call_queue_t *q, ui_call_fn_t fn, void *arg, ui_call_async_t cancel_fallback);
// The drain hand_over schedules (arg is the queue), on the LVGL task with the lock held: runs the batch queued before it
// started, oldest first, skipping the cancelled ones.
void ui_call_queue_drain(void *queue);
