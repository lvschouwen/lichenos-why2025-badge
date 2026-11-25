#include "video_notice.h"

#include <stdatomic.h>

static atomic_bool                 s_active;
static _Atomic(bool (*)(vn_kind_t)) s_reclaim;

vn_action_t video_notice_policy(vn_kind_t kind, bool owner_active) {
    if (!owner_active) {
        return VN_SHOW;
    }
    return kind == VN_TOAST ? VN_HOLD : VN_RECLAIM;
}

void video_notice_set_active(bool active) {
    atomic_store(&s_active, active);
}

void video_notice_set_reclaim(bool (*reclaim)(vn_kind_t kind)) {
    atomic_store(&s_reclaim, reclaim);
}

bool video_notice_active(void) {
    return atomic_load(&s_active);
}

bool video_notice_raise(vn_kind_t kind) {
    bool (*reclaim)(vn_kind_t) = atomic_load(&s_reclaim);
    if (!reclaim || video_notice_policy(kind, video_notice_active()) != VN_RECLAIM) {
        return false;
    }
    return reclaim(kind);
}

bool video_escape_sample(video_escape_t *e, bool active, bool down, uint32_t presses, uint32_t now) {
    if (!active) {
        *e = (video_escape_t){.seen = presses};
        return false;
    }
    if (!e->was_active) { // the owner just started: a key already down is no escape, only a new press counts
        e->was_active = true;
        e->seen       = presses;
    }
    if (presses != e->seen) {
        e->seen  = presses;
        e->armed = true;
        e->fired = false;
        e->since = now;
    }
    if (!down) {
        e->armed = false;
    }
    if (!e->armed || e->fired || (uint32_t)(now - e->since) < VIDEO_ESCAPE_MS) {
        return false;
    }
    e->fired = true;
    return true;
}
