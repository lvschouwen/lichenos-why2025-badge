// lichen_video.h for apps (#562): the app-facing calls over the OS video surface (video_owner.h). The decisions are
// lichen_video_rules.c's (host-tested); this binds them to the capability check and the owner record.
#include "lichen_video.h"
#include "lichen_sys_app.h"
#include "lichen_video_rules.h"
#include "video_owner.h"

static lichen_video_t s_video; // one holder at a time, like the surface

static bool cap_ok(void) {
    return lichen_sys_caps_self_ok(CAP_VIDEO, "lichen_video");
}

static uint32_t self(void) {
    return lichen_ctx_self();
}

static bool holds(uint32_t ctx) {
    return video_owner_holds(ctx);
}

static lichen_video_ops_t const OPS = {
    .cap_ok  = cap_ok,
    .self    = self,
    .acquire = video_owner_acquire,
    .present = video_owner_present,
    .release = video_owner_release,
    .holds   = holds,
};

static void reclaimed(void *arg, display_video_reclaim_t why) {
    (void)arg;
    lichen_video_do_reclaimed(&s_video, why);
}

bool lichen_video_acquire(lichen_video_cfg_t const *cfg) {
    return lichen_video_do_acquire(&s_video, &OPS, cfg, reclaimed);
}

uint16_t *lichen_video_back(void) {
    return lichen_video_do_back(&s_video, &OPS);
}

bool lichen_video_present(void) {
    return lichen_video_do_present(&s_video, &OPS);
}

bool lichen_video_release(void) {
    return lichen_video_do_release(&s_video, &OPS);
}
