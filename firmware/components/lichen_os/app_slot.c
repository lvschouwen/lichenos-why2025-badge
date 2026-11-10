#include "app_slot.h"

size_t app_slot_pick(void const *const owners[], size_t n, void const *app) {
    size_t free_slot = n;
    for (size_t i = 0; i < n; i++) {
        if (owners[i] == app) {
            return i; // its own slot wins over a free one before it (a slot freed earlier must not give it two)
        }
        if (free_slot == n && !owners[i]) {
            free_slot = i;
        }
    }
    return free_slot;
}
