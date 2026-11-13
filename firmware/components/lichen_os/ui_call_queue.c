#include "ui_call_queue.h"

ui_call_push_t ui_call_queue_push(ui_call_queue_t *q, ui_call_fn_t fn, void *arg, uint32_t owner) {
    if (q->count >= UI_CALL_QUEUE_CAP) {
        return UI_CALL_FULL;
    }
    q->slot[(q->head + q->count) % UI_CALL_QUEUE_CAP] = (ui_call_t){fn, arg, owner, key_cause_now()};
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

size_t ui_call_queue_cancel(ui_call_queue_t *q, ui_call_fn_t fn, void *arg) {
    size_t cancelled = 0;
    for (uint8_t i = 0; i < q->count; i++) {
        ui_call_t *c = &q->slot[(q->head + i) % UI_CALL_QUEUE_CAP];
        if (fn && c->fn == fn && c->arg == arg) {
            c->fn = NULL; // left in place: a drain that is running counted it in its batch
            cancelled++;
        }
    }
    return cancelled;
}

void ui_call_queue_cancel_all(ui_call_queue_t *q, ui_call_fn_t fn, void *arg, ui_call_async_t cancel_fallback) {
    if (!fn) {
        return;
    }
    ui_call_queue_cancel(q, fn, arg);
    cancel_fallback(fn, arg); // a burst past the queue went to the fallback: it may hold this pair too
}

bool ui_call_queue_hand_over(ui_call_queue_t *q, ui_call_fn_t fn, void *arg, uint32_t owner, ui_call_async_t schedule,
                             ui_call_async_t fallback) {
    switch (ui_call_queue_push(q, fn, arg, owner)) {
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
        if (!call.fn) {
            continue;
        }
        key_cause_t previous = key_cause_enter(call.cause);
        if (q->run) {
            q->run(call.fn, call.arg, call.owner);
        } else {
            call.fn(call.arg);
        }
        key_cause_leave(previous);
    }
}
