#include "dev_tap.h"

void dev_tap_reset(dev_tap_t *t) {
    t->n    = 0;
    t->last = 0;
}

bool dev_tap(dev_tap_t *t, uint32_t now) {
    if (t->n > 0 && (uint32_t)(now - t->last) > DEV_TAP_GAP_MS) {
        t->n = 0;
    }
    t->last = now;
    if (++t->n >= DEV_TAP_COUNT) {
        t->n = 0;
        return true;
    }
    return false;
}
