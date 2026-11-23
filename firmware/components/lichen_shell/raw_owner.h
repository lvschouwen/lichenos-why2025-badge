#pragma once

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
