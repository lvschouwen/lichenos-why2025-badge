#pragma once

#include "key_raw.h"

#include <stdbool.h>
#include <stdint.h>

// Who holds the raw keys (#534): one app at a time, by its context. Only the Keyboard test marks the screen as the
// Keyboard test (fw keys log, ui_remote's message); any other holder, a game, just owns the keys. Pure (no ESP),
// host-tested in host_tests/raw_owner_test.c; raw_keys.c keeps the one instance and does the listening.
typedef struct {
    uint32_t ctx; // lichen_ctx_t of the holder; 0 = nobody
    bool     keytest;
} raw_owner_t;

// False when somebody holds the keys already or ctx is 0
bool raw_owner_take(raw_owner_t *o, uint32_t ctx, bool is_keytest);
// True when ctx held the keys, which are then free; false (and nothing changes) for any other ctx
bool raw_owner_release_ctx(raw_owner_t *o, uint32_t ctx);
bool raw_owner_held(raw_owner_t const *o);
// Whether the screen counts as the Keyboard test: held, and by the Keyboard test
bool raw_owner_test_mark(raw_owner_t const *o);

// The caller is the Keyboard test: its ctx is the one registered for it (neither may be 0)
bool raw_owner_is_keytest(uint32_t caller, uint32_t keytest_ctx);
// What the keyboard task stamps into each raw event at capture: the mark and the held flag as the owner left them
void raw_owner_stamp(key_raw_event_t *ev, bool test_mark, bool held);
// Whether a stamped event is for an app that owns the keys (the shape-key shortcuts stay quiet): any holder, not only
// the Keyboard test
bool raw_owner_event_owned(key_raw_event_t const *ev);
// Why a remote "ui open" is refused while raw keys are held: NULL when they are not
char const *raw_owner_refusal(bool held, bool test_mark);
