// The cause of the UI work running now (#400): the shell's mark, else the last LVGL key's origin
#include "key_cause.h"

static key_cause_last_fn bound_last; // atomic: the shell binds it once, any task may
static key_cause_t       mark;       // under the LVGL lock

void key_cause_bind_last_key(key_cause_last_fn fn) {
    __atomic_store_n(&bound_last, fn, __ATOMIC_RELEASE);
}

key_cause_t key_cause_enter(key_cause_t cause) {
    key_cause_t previous = mark;
    mark                 = cause;
    return previous;
}

void key_cause_leave(key_cause_t previous) {
    mark = previous;
}

key_cause_t key_cause_now(void) {
    if (mark != KEY_CAUSE_NONE) {
        return mark;
    }
    key_cause_last_fn fn = __atomic_load_n(&bound_last, __ATOMIC_ACQUIRE);
    return fn && !fn() ? KEY_CAUSE_HAND : KEY_CAUSE_INJECTED; // unbound: no key is known to be a hand
}

bool key_cause_last_key_injected(bool indev_active, bool last_injected) {
    return !indev_active || last_injected; // outside a read, the key read last is not what set this work off
}

bool key_cause_hand_only_ok(key_cause_t cause) {
    return cause == KEY_CAUSE_HAND || cause == KEY_CAUSE_SYSTEM;
}

char const *key_cause_name(key_cause_t cause) {
    switch (cause) {
        case KEY_CAUSE_HAND: return "hand";
        case KEY_CAUSE_INJECTED: return "injected key";
        case KEY_CAUSE_SYSTEM: return "system";
        default: return "none";
    }
}
