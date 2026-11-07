#include "ui_call_queue.h"

ui_call_push_t ui_call_queue_push(ui_call_queue_t *q, ui_call_fn_t fn, void *arg) {
    if (q->count >= UI_CALL_QUEUE_CAP) {
        return UI_CALL_FULL;
    }
    q->slot[(q->head + q->count) % UI_CALL_QUEUE_CAP] = (ui_call_t){fn, arg};
    q->count++;
    if (q->scheduled) {
        return UI_CALL_QUEUED;
    }
    q->scheduled = true;
    return UI_CALL_QUEUED_SCHEDULE;
}

void ui_call_queue_unpush(ui_call_queue_t *q) {
    if (q->count > 0) {
        q->count--;
    }
    // Only the push that set it calls this, and no drain started in between (the lock is held throughout)
    q->scheduled = false;
}

size_t ui_call_queue_begin_drain(ui_call_queue_t *q) {
    q->scheduled = false;
    return q->count;
}

bool ui_call_queue_pop(ui_call_queue_t *q, ui_call_t *out) {
    if (q->count == 0) {
        return false;
    }
    *out    = q->slot[q->head];
    q->head = (uint8_t)((q->head + 1) % UI_CALL_QUEUE_CAP);
    q->count--;
    return true;
}

bool ui_call_queue_hand_over(ui_call_queue_t *q, ui_call_fn_t fn, void *arg, ui_call_async_t schedule,
                             ui_call_async_t fallback) {
    switch (ui_call_queue_push(q, fn, arg)) {
    case UI_CALL_QUEUED:
        return true;
    case UI_CALL_QUEUED_SCHEDULE:
        if (schedule(ui_call_queue_drain, q)) {
            return true;
        }
        ui_call_queue_unpush(q); // else the next push finds a drain "on its way" that never comes: calls stranded
        return false;
    case UI_CALL_FULL:
    default:
        return fallback(fn, arg);
    }
}

void ui_call_queue_drain(void *queue) {
    ui_call_queue_t *q = queue;
    size_t           n = ui_call_queue_begin_drain(q);
    ui_call_t        call;
    while (n-- > 0 && ui_call_queue_pop(q, &call)) {
        call.fn(call.arg);
    }
}
