#include "lora_regq.h"

#include <string.h>

void lora_regq_clear(lora_regq_t *q) {
    memset(q->req, 0, sizeof(q->req));
    q->head  = 0;
    q->count = 0;
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

bool lora_regq_take(lora_regq_t *q, lora_regq_req_t *out) {
    if (!q->count) {
        return false;
    }
    *out    = q->req[q->head];
    q->head = (uint8_t)((q->head + 1) % LORA_REGQ_MAX);
    q->count--;
    return true;
}
