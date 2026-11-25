#include "lichen_video_rules.h"

#include <string.h>

// An app's call needs VIDEO and, but for acquire, a record that is still the owner's: an unload sweep frees the sources
// without telling this record, so a stale pointer must never reach a later VIDEO app.
// Only the holder acts on the record: another app with VIDEO is refused (counted), not served.
static bool mine(lichen_video_t const *v, lichen_video_ops_t const *ops, char const *what) {
    if (!v->src[0]) {
        return false;
    }
    if (v->ctx != ops->self()) {
        ops->refuse(what);
        return false;
    }
    return true;
}

static bool live(lichen_video_t const *v, lichen_video_ops_t const *ops, char const *what) {
    return ops->cap_ok() && mine(v, ops, what) && ops->holds(v->ctx);
}

bool lichen_video_do_acquire(lichen_video_t *v, lichen_video_ops_t const *ops, lichen_video_cfg_t const *cfg,
                             void (*on_reclaim)(void *, display_video_reclaim_t)) {
    if (!ops->cap_ok() || !cfg || cfg->src_w == 0 || cfg->src_h == 0) {
        return false;
    }
    void    *src[2] = {NULL, NULL};
    uint32_t ctx    = ops->self();
    if (ops->acquire(ctx, cfg->src_w, cfg->src_h, LICHEN_VIDEO_SCALE_X_MILLI, LICHEN_VIDEO_SCALE_Y_MILLI, on_reclaim, NULL,
                     src) != DISPLAY_VIDEO_REFUSE_NONE) {
        return false;
    }
    // First frame into source 1: source 0 is the black one the surface shows until the first present (video_owner.h)
    *v = (lichen_video_t){.ctx = ctx, .src = {src[0], src[1]}, .back = 1, .cb = cfg->on_reclaim, .cb_arg = cfg->arg};
    return true;
}

uint16_t *lichen_video_do_back(lichen_video_t *v, lichen_video_ops_t const *ops) {
    return live(v, ops, "lichen_video_back") ? (uint16_t *)v->src[v->back] : NULL;
}

bool lichen_video_do_present(lichen_video_t *v, lichen_video_ops_t const *ops) {
    if (!live(v, ops, "lichen_video_present") || !ops->present(v->ctx, v->back)) {
        return false; // dropped: the other source is the pending one, so the same back buffer is free to rewrite
    }
    v->back ^= 1u;
    return true;
}

bool lichen_video_do_release(lichen_video_t *v, lichen_video_ops_t const *ops) {
    if (!ops->cap_ok()) {
        return false;
    }
    if (!v->src[0]) {
        return true; // nothing held counts as done
    }
    if (!mine(v, ops, "lichen_video_release")) {
        return true; // another app's record: a no-op, as if there were nothing of the caller's to release
    }
    if (!ops->release(v->ctx)) {
        return false; // a lock timeout keeps the record for the retry
    }
    memset(v, 0, sizeof(*v));
    return true;
}

void lichen_video_do_reclaimed(lichen_video_t const *v, display_video_reclaim_t why) {
    if (v->cb && why > DISPLAY_VIDEO_RECLAIM_NONE && why < DISPLAY_VIDEO_RECLAIM_COUNT) {
        v->cb(v->cb_arg, (lichen_video_reclaim_t)why);
    }
}
