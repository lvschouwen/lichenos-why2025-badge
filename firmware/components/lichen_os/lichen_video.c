// lichen_video.h for apps (#562): the app-facing calls over the OS video surface (video_owner.h). The decisions are
// lichen_video_rules.c's (host-tested); this binds them to the capability check and the owner record.
#include "lichen_video.h"
#include "lichen_sys_app.h"
#include "lichen_video_rules.h"
#include "video_owner.h"

#include "esp_log.h"

static lichen_video_t s_video; // one holder at a time, like the surface

static bool cap_ok(void) {
    return lichen_sys_caps_self_ok(CAP_VIDEO, "lichen_video");
}

static uint32_t self(void) {
    lichen_ctx_t c = lichen_ctx_self();
    return c == lichen_sys_ctx_os() ? LICHEN_CTX_NONE : c; // the OS and unbound tasks are no holder
}

static void refuse(char const *what) {
    static uint32_t n;
    uint32_t        c = __atomic_fetch_add(&n, 1u, __ATOMIC_RELAXED);
    if ((c & 0xFFu) == 0) { // per frame calls: the first, then every 256th
        ESP_LOGW("video", "%s refused: the screen is another app's (%lu so far)", what, (unsigned long)c + 1u);
    }
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
    .refuse  = refuse,
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
