#include "lora_regq.h"

#include <string.h>

void lora_regq_clear(lora_regq_t *q) {
    memset(q->req, 0, sizeof(q->req));
    q->head         = 0;
    q->count        = 0;
    q->wake_pending = false; // the receive queue is reset with it
    q->n_fly        = 0;     // nobody waits for those answers any more
}

bool lora_regq_want_wake(lora_regq_t *q) {
    if (q->wake_pending) {
        return false;
    }
    q->wake_pending = true;
    return true;
}

void lora_regq_woken(lora_regq_t *q) {
    q->wake_pending = false;
}

uint32_t lora_regq_settle_ms(bool set_sent, uint32_t set_sent_ms, uint32_t now_ms) {
    return set_sent && now_ms - set_sent_ms < LORA_REGQ_SETTLE_RECENT_MS ? LORA_REGQ_SETTLE_MS : 0;
}

static bool blocked(lora_regq_t const *q, uint16_t id) {
    for (uint8_t i = 0; i < q->n_blocked; i++) {
        if (q->blocked[i] == id) {
            return true;
        }
    }
    return false;
}

bool lora_regq_block(lora_regq_t *q, uint16_t id) {
    uint8_t kept = 0;
    for (uint8_t i = 0; i < q->count; i++) {
        lora_regq_req_t r = q->req[(q->head + i) % LORA_REGQ_MAX];
        if (r.id != id) {
            q->req[(q->head + kept++) % LORA_REGQ_MAX] = r;
        }
    }
    q->count = kept;
    if (blocked(q, id)) {
        return true;
    }
    if (q->n_blocked == LORA_REGQ_BLOCK_MAX) {
        return false;
    }
    q->blocked[q->n_blocked++] = id;
    return true;
}

bool lora_regq_post(lora_regq_t *q, lora_regq_req_t const *req) {
    if (blocked(q, req->id)) {
        return false;
    }
    for (uint8_t i = 0; i < q->count; i++) {
        lora_regq_req_t *r = &q->req[(q->head + i) % LORA_REGQ_MAX];
        if (r->id != req->id) {
            continue;
        }
        if (req->set) {
            r->set   = true;
            r->value = req->value;
        }
        r->seq = req->seq;
        return true;
    }
    if (q->count == LORA_REGQ_MAX) {
        return false;
    }
    q->req[(q->head + q->count) % LORA_REGQ_MAX] = *req;
    q->count++;
    return true;
}

static void fly_drop(lora_regq_t *q, uint8_t i) {
    q->fly[i] = q->fly[--q->n_fly];
}

static void fly_expire(lora_regq_t *q, uint32_t now_ms) {
    for (uint8_t i = q->n_fly; i-- > 0;) {
        if (now_ms - q->fly[i].sent_ms >= LORA_REGQ_ANSWER_MS) {
            fly_drop(q, i);
            q->timeouts++;
        }
    }
}

// After this request goes out: the waiting messages, every answer still to come (its own included) and one wake token
// must leave LORA_REGQ_RX_ROOM places for RX_PACKET, STATUS and RESULT
static bool rx_room(lora_regq_t const *q, size_t rx_waiting, size_t rx_depth) {
    size_t taken = rx_waiting + q->n_fly + 1 + 1;
    return q->n_fly < LORA_REGQ_IN_FLIGHT && taken <= rx_depth && rx_depth - taken >= LORA_REGQ_RX_ROOM;
}

bool lora_regq_take(lora_regq_t *q, uint32_t now_ms, size_t rx_waiting, size_t rx_depth, lora_regq_req_t *out) {
    fly_expire(q, now_ms);
    if (!q->count || !rx_room(q, rx_waiting, rx_depth)) {
        return false;
    }
    *out    = q->req[q->head];
    q->head = (uint8_t)((q->head + 1) % LORA_REGQ_MAX);
    q->count--;
    q->fly[q->n_fly++] = (lora_regq_fly_t){.id = out->id, .seq = out->seq, .sent_ms = now_ms};
    return true;
}

void lora_regq_sent(lora_regq_t *q, uint16_t id, uint8_t seq, uint32_t now_ms) {
    for (uint8_t i = 0; i < q->n_fly; i++) {
        if (q->fly[i].id == id && q->fly[i].seq == seq) {
            q->fly[i].sent_ms = now_ms;
        }
    }
}

bool lora_regq_answered(lora_regq_t *q, uint16_t id, uint8_t seq) {
    for (uint8_t i = 0; i < q->n_fly; i++) {
        if (q->fly[i].id == id && q->fly[i].seq == seq) {
            fly_drop(q, i);
            return true;
        }
    }
    return false;
}

uint32_t lora_regq_wait_ms(lora_regq_t const *q, uint32_t now_ms, uint32_t wait_ms) {
    if (!q->count || !q->n_fly) {
        return wait_ms; // nothing held back by a request in flight: a post, an answer or a message wakes the task
    }
    uint32_t soonest = wait_ms;
    for (uint8_t i = 0; i < q->n_fly; i++) {
        uint32_t age  = now_ms - q->fly[i].sent_ms;
        uint32_t left = age >= LORA_REGQ_ANSWER_MS ? 0 : LORA_REGQ_ANSWER_MS - age;
        soonest       = left < soonest ? left : soonest;
    }
    return soonest;
}

size_t lora_regq_ahead(lora_regq_t const *q) {
    return (size_t)q->count + q->n_fly;
}

uint32_t lora_regq_burst_ms(size_t n) {
    // Every full window before the last may time out once; the last one's answers are the caller's own wait
    size_t windows = n ? (n - 1) / LORA_REGQ_IN_FLIGHT : 0;
    return (uint32_t)(windows * LORA_REGQ_ANSWER_MS);
}
