// notify_push.h: the notification service's state and feedback pushes, kept pure for the host test (#245)
#include "notify_push.h"

#include <string.h>

void npush_bind(npush_t *p, ln_state_hooks_t const *hooks) {
    if (hooks) {
        p->hooks = *hooks;
    } else {
        memset(&p->hooks, 0, sizeof(p->hooks));
    }
    npush_dnd(p, p->dnd);
    npush_unread(p, p->unread);
}

void npush_dnd(npush_t *p, bool on) {
    p->dnd = on;
    if (p->hooks.dnd) {
        p->hooks.dnd(on);
    }
}

void npush_unread(npush_t *p, bool any) {
    p->unread = any;
    if (p->hooks.unread) {
        p->hooks.unread(any);
    }
}

void npush_feedback(ln_feedback_t const *fb, ln_level_t level, bool on) {
    if (on && fb->sound) {
        fb->sound(level);
    }
    if (on && fb->vibrate) {
        fb->vibrate(level);
    }
    if (fb->led) {
        fb->led(level, on);
    }
}
