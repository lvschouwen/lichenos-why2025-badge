#include "key_origin.h"

key_origin_t key_origin_of_inject(bool as_physical) {
    return as_physical ? KEY_ORIGIN_INJECTED_GATE : KEY_ORIGIN_INJECTED;
}

bool key_origin_gated(key_origin_t o) {
    return o != KEY_ORIGIN_INJECTED;
}

bool key_origin_injected(key_origin_t o) {
    return o != KEY_ORIGIN_CHIP; // an unknown value counts as injected: only the chip is a hand
}
