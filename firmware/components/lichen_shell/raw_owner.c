#include "raw_owner.h"

#include <stddef.h>

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

bool raw_owner_is_keytest(uint32_t caller, uint32_t keytest_ctx) {
    return caller != 0 && caller == keytest_ctx;
}

void raw_owner_stamp(key_raw_event_t *ev, bool test_mark, bool held) {
    ev->test_open = test_mark;
    ev->held      = held;
}

bool raw_owner_event_owned(key_raw_event_t const *ev) {
    return ev->held;
}

char const *raw_owner_refusal(bool held, bool test_mark) {
    if (!held) {
        return NULL;
    }
    return test_mark ? "the Keyboard test is open" : "an app holds the raw keys";
}
