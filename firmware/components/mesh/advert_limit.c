#include "advert_limit.h"

#include <string.h>

void advert_limit_init(advert_limit_t *l) {
    memset(l, 0, sizeof(*l));
    l->tokens = ADVERT_LIMIT_BURST;
}

static void top_up(advert_limit_t *l, uint32_t now_ms) {
    if (!l->primed) {
        l->primed  = 1;
        l->last_ms = now_ms;
        return;
    }
    uint32_t steps = (now_ms - l->last_ms) / ADVERT_LIMIT_EVERY_MS; // unsigned: right across the wrap
    if (steps == 0) {
        return;
    }
    if (steps >= ADVERT_LIMIT_BURST - l->tokens) {
        l->tokens  = ADVERT_LIMIT_BURST;
        l->last_ms = now_ms; // full: quiet time is not saved up
    } else {
        l->tokens  += steps;
        l->last_ms += steps * ADVERT_LIMIT_EVERY_MS; // the rest of an interval counts towards the next token
    }
}

advert_limit_result_t advert_limit_take(advert_limit_t *l, uint32_t now_ms) {
    top_up(l, now_ms);
    if (l->tokens > 0) {
        l->tokens--;
        l->limiting = 0;
        return ADVERT_LIMIT_OK;
    }
    l->refused++;
    if (l->limiting) {
        return ADVERT_LIMIT_REFUSED;
    }
    l->limiting = 1;
    return ADVERT_LIMIT_REFUSED_FIRST;
}
