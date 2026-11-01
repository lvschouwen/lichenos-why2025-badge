// Per-source token bucket for notifications (#62): see notify_rate.h. Pure, host-tested in host_tests/notify_rate_test.c.
#include "notify_rate.h"

#include <string.h>

#define LEVEL_ERROR 2

static int find_slot(nrate_t *t, char const *source, uint32_t now_ms) {
    int free_at = -1, oldest = 0;
    for (int i = 0; i < NRATE_SLOTS; i++) {
        if (t->slot[i].source[0] && strncmp(t->slot[i].source, source, NRATE_SOURCE_CAP - 1) == 0) {
            return i;
        }
        if (!t->slot[i].source[0] && free_at < 0) {
            free_at = i;
        }
        if (t->clock - t->slot[i].seen > t->clock - t->slot[oldest].seen) {
            oldest = i;
        }
    }
    int i = free_at >= 0 ? free_at : oldest;
    memset(&t->slot[i], 0, sizeof(t->slot[i]));
    for (int k = 0; k < NRATE_SOURCE_CAP - 1 && source[k]; k++) {
        t->slot[i].source[k] = source[k];
    }
    t->slot[i].tokens    = NRATE_BURST;
    t->slot[i].refill_ms = now_ms;
    return i;
}

bool nrate_take(nrate_t *t, char const *source, uint32_t now_ms) {
    int i = find_slot(t, source ? source : "", now_ms);
    t->slot[i].seen = ++t->clock;
    uint32_t gained = (now_ms - t->slot[i].refill_ms) / NRATE_REFILL_MS;
    if (gained) {
        uint32_t tokens = t->slot[i].tokens + gained;
        if (tokens >= NRATE_BURST) {
            t->slot[i].tokens    = NRATE_BURST;
            t->slot[i].refill_ms = now_ms;
        } else {
            t->slot[i].tokens = (uint8_t)tokens;
            t->slot[i].refill_ms += gained * NRATE_REFILL_MS;
        }
    }
    if (t->slot[i].tokens == 0) {
        return false;
    }
    if (t->slot[i].tokens == NRATE_BURST) {
        t->slot[i].refill_ms = now_ms; // a full bucket starts refilling from its first use, not from long ago
    }
    t->slot[i].tokens--;
    return true;
}

nrate_show_t nrate_show(uint8_t level, bool silent, bool dnd, bool within_rate) {
    nrate_show_t show = {false, false};
    if (silent || !within_rate) {
        return show;
    }
    if (dnd) {
        show.toast = level >= LEVEL_ERROR;
        return show;
    }
    show.toast    = true;
    show.feedback = true;
    return show;
}
