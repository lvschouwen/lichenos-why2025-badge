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
// The oldest call, taken off the queue before it runs (so it may queue again). False when the queue is empty.
bool ui_call_queue_pop(ui_call_queue_t *q, ui_call_t *out);
