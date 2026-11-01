// Per-source notification switch (#62): see notify_mute.h. Pure, host-tested in host_tests/notify_mute_test.c.
#include "notify_mute.h"

#include <stdio.h>
#include <string.h>

bool nmute_key(char const *source, char *out, size_t cap) {
    if (!source || !source[0] || strlen(source) > NMUTE_SOURCE_CAP - 1) {
        return false;
    }
    int n = snprintf(out, cap, "n.%s", source);
    return n > 0 && (size_t)n < cap && n < NMUTE_KEY_CAP;
}

static int find(nmute_t const *t, char const *source) {
    for (size_t i = 0; i < t->used; i++) {
        if (strcmp(t->slot[i].source, source) == 0) {
            return (int)i;
        }
    }
    return -1;
}

nmute_state_t nmute_get(nmute_t const *t, char const *source) {
    int i = source ? find(t, source) : -1;
    return i < 0 ? NMUTE_UNKNOWN : t->slot[i].on ? NMUTE_ON : NMUTE_OFF;
}

void nmute_put(nmute_t *t, char const *source, bool on) {
    if (!source || strlen(source) > NMUTE_SOURCE_CAP - 1) {
        return;
    }
    int i = find(t, source);
    if (i < 0) {
        if (t->used == NMUTE_SLOTS) {
            memmove(&t->slot[0], &t->slot[1], sizeof(t->slot[0]) * (NMUTE_SLOTS - 1));
            t->used--;
        }
        i = (int)t->used++;
        snprintf(t->slot[i].source, sizeof(t->slot[i].source), "%s", source);
    }
    t->slot[i].on = on;
}

bool nmute_drop(nmute_t *t, char const *source) {
    if (nmute_get(t, source) != NMUTE_OFF) {
        return false; // on, or never known: default on
    }
    t->muted++;
    return true;
}
