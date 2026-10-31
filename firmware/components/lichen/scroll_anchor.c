#include "scroll_anchor.h"

scroll_anchor_t scroll_anchor_take(scroll_item_t const *items, size_t n, int32_t scroll_y) {
    scroll_anchor_t a = {0};
    for (size_t i = 0; i < n && a.n < SCROLL_ANCHOR_KEYS; i++) {
        if (a.n == 0 && items[i].bottom <= scroll_y) {
            continue; // above the view
        }
        a.key[a.n]    = items[i].key;
        a.offset[a.n] = items[i].top - scroll_y;
        a.n++;
    }
    return a;
}

bool scroll_anchor_find(scroll_anchor_t const *a, scroll_item_t const *items, size_t n, int32_t *scroll_y) {
    for (size_t k = 0; k < a->n; k++) {
        for (size_t i = 0; i < n; i++) {
            if (items[i].key == a->key[k]) {
                *scroll_y = items[i].top - a->offset[k];
                return true;
            }
        }
    }
    return false;
}

uint32_t scroll_key_hash(void const *data, size_t len, uint32_t seed) {
    uint8_t const *p = data;
    uint32_t       h = seed ^ 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}
