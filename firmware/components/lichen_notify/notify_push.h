#pragma once
// The pushing half of notify_bind.h's state hooks and of the feedback hooks (#245): the last state, who is told, and
// which feedback members fire. Pure (no ESP headers), host-tested in host_tests/notify_push_test.c; lichen_notify.c
// calls it under its lock (unread) or its push critical section (dnd).

#include "notify_bind.h"
#include "notify_os.h"

#include <stdbool.h>

typedef struct {
    ln_state_hooks_t hooks;
    bool             dnd;
    bool             unread;
} npush_t;

// Takes the hooks (NULL: none) and tells them the current state at once
void npush_bind(npush_t *p, ln_state_hooks_t const *hooks);
// Records the new value and pushes it, also when it did not change (a hook only stores it)
void npush_dnd(npush_t *p, bool on);
void npush_unread(npush_t *p, bool any);

// lichen_notify_feedback's dispatch: on fires sound, then vibrate; led gets on and off. A NULL member is skipped.
void npush_feedback(ln_feedback_t const *fb, ln_level_t level, bool on);
