#include "video_notice.h"

#include <stdatomic.h>

static atomic_bool                 s_active;
static atomic_uint                 s_owner;
static _Atomic(void (*)(uint32_t)) s_keys_release;
static _Atomic(bool (*)(vn_kind_t)) s_reclaim;

vn_action_t video_notice_policy(vn_kind_t kind, bool owner_active) {
    if (!owner_active) {
        return VN_SHOW;
    }
    return kind == VN_TOAST ? VN_HOLD : VN_RECLAIM;
}

void video_notice_set_active(bool active, uint32_t owner_ctx) {
    atomic_store(&s_owner, active ? owner_ctx : 0);
    atomic_store(&s_active, active);
}

uint32_t video_notice_owner(void) {
    return atomic_load(&s_active) ? atomic_load(&s_owner) : 0;
}

void video_notice_set_keys_release(void (*release)(uint32_t ctx)) {
    atomic_store(&s_keys_release, release);
}

void video_notice_reclaimed(void) {
    uint32_t ctx = video_notice_owner();
    video_notice_set_active(false, 0);
    void (*release)(uint32_t) = atomic_load(&s_keys_release);
    if (ctx && release) {
        release(ctx);
    }
}

bool video_notice_toast_may_show(bool showing, unsigned waiting, bool home_shown, bool update_busy, bool owner_active) {
    return !showing && waiting && home_shown && !update_busy && video_notice_policy(VN_TOAST, owner_active) != VN_HOLD;
}

void video_escape_retry(video_escape_t *e) {
    e->fired = false;
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
