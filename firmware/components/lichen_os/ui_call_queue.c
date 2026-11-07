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
