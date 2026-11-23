#include "raw_owner.h"

bool raw_owner_take(raw_owner_t *o, uint32_t ctx, bool is_keytest) {
    if (ctx == 0 || o->ctx != 0) {
        return false;
    }
    o->ctx     = ctx;
    o->keytest = is_keytest;
    return true;
}

bool raw_owner_release_ctx(raw_owner_t *o, uint32_t ctx) {
    if (ctx == 0 || o->ctx != ctx) {
        return false;
    }
    o->ctx     = 0;
    o->keytest = false;
    return true;
}

bool raw_owner_held(raw_owner_t const *o) {
    return o->ctx != 0;
}

bool raw_owner_test_mark(raw_owner_t const *o) {
    return o->ctx != 0 && o->keytest;
}
