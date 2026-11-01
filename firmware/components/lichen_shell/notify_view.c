// What the shell shows of notifications (#62): see notify_view.h. Pure, host-tested in host_tests/notify_view_test.c.
#include "notify_view.h"

#include <stdio.h>
#include <string.h>

uint32_t nview_toast_ms(uint8_t level) {
    return level >= 2 ? 10000 : level == 1 ? 6000 : 3000;
}

bool nview_push(nview_queue_t *q, nview_toast_t t) {
    for (size_t i = 0; i < q->n; i++) {
        if (q->q[i].id == t.id) {
            q->q[i].level    = t.level > q->q[i].level ? t.level : q->q[i].level;
            q->q[i].feedback = q->q[i].feedback || t.feedback;
            return true;
        }
    }
    if (q->n == NVIEW_QUEUE) {
        size_t low = 0;
        for (size_t i = 1; i < q->n; i++) {
            if (q->q[i].level < q->q[low].level) {
                low = i; // the first of the lowest level is its oldest
            }
        }
        if (q->q[low].level > t.level) {
            return false;
        }
        memmove(&q->q[low], &q->q[low + 1], (q->n - low - 1) * sizeof(q->q[0]));
        q->n--;
    }
    q->q[q->n++] = t;
    return true;
}

bool nview_pop(nview_queue_t *q, nview_toast_t *out) {
    if (!q->n) {
        return false;
    }
    *out = q->q[0];
    memmove(&q->q[0], &q->q[1], (q->n - 1) * sizeof(q->q[0]));
    q->n--;
    return true;
}

void nview_forget(nview_queue_t *q, uint32_t id) {
    for (size_t i = 0; i < q->n; i++) {
        if (q->q[i].id == id) {
            memmove(&q->q[i], &q->q[i + 1], (q->n - i - 1) * sizeof(q->q[0]));
            q->n--;
            return;
        }
    }
}

void nview_title(char *out, size_t cap, char const *title, unsigned count) {
    if (count > 1) {
        snprintf(out, cap, "%s (\xC3\x97%u)", title, count);
    } else {
        snprintf(out, cap, "%s", title);
    }
}

static void span(char *out, size_t cap, uint64_t s) {
    if (s < 60) {
        snprintf(out, cap, "now");
    } else if (s < 3600) {
        snprintf(out, cap, "%u min", (unsigned)(s / 60));
    } else if (s < 86400) {
        snprintf(out, cap, "%u h", (unsigned)(s / 3600));
    } else {
        snprintf(out, cap, "%u d", (unsigned)(s / 86400));
    }
}

void nview_age(char *out, size_t cap, uint16_t boot, uint32_t mono_s, int64_t epoch, uint16_t now_boot,
               uint32_t now_mono_s, int64_t now_epoch) {
    if (boot == now_boot && now_mono_s >= mono_s) {
        span(out, cap, now_mono_s - mono_s);
    } else if (epoch > 0 && now_epoch >= epoch) {
        span(out, cap, (uint64_t)(now_epoch - epoch));
    } else {
        snprintf(out, cap, "earlier boot");
    }
}

void nview_count(char *out, size_t cap, size_t unread) {
    if (!unread) {
        snprintf(out, cap, "%s", "");
    } else if (unread > 99) {
        snprintf(out, cap, "99+");
    } else {
        snprintf(out, cap, "%u", (unsigned)unread);
    }
}
