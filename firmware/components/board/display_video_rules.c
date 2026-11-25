#include "display_video_rules.h"

display_video_refusal_t display_video_acquire_verdict(display_video_acquire_in_t const *in) {
    if (in->owned) {
        return DISPLAY_VIDEO_REFUSE_OWNED;
    }
    if (!in->engine_ppa || in->engine_hung || !in->swap_on) {
        return DISPLAY_VIDEO_REFUSE_ENGINE;
    }
    if (in->psram_mhz < DISPLAY_VIDEO_PSRAM_MIN_MHZ) {
        return DISPLAY_VIDEO_REFUSE_PSRAM;
    }
    if (!in->geom_ok || in->src_w > DISPLAY_VIDEO_SRC_SIDE_MAX || in->src_h > DISPLAY_VIDEO_SRC_SIDE_MAX ||
        (uint64_t)in->src_w * in->src_h * 2 > DISPLAY_VIDEO_SRC_BYTES_MAX) {
        return DISPLAY_VIDEO_REFUSE_GEOM;
    }
    return in->mem_ok ? DISPLAY_VIDEO_REFUSE_NONE : DISPLAY_VIDEO_REFUSE_MEM;
}

char const *display_video_refusal_name(display_video_refusal_t r) {
    static char const *const names[] = {"none", "owned", "engine", "psram-80mhz", "geometry", "memory", "ui-busy"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

int display_video_pump_pick(int pending, int last, uint32_t bands, uint32_t since_last_us) {
    if (pending >= 0) {
        return pending;
    }
    if (bands == 0) {
        return -1;
    }
    if (last < 0) {
        return 0;
    }
    return since_last_us >= DISPLAY_VIDEO_KEEPALIVE_US ? last : -1;
}

bool display_video_console_stale(bool have, bool held, bool reclaimed) {
    return have && (!held || reclaimed);
}
