#include "display_video_state.h"

void display_video_owner_init(display_video_owner_t *o) {
    *o = (display_video_owner_t){.state = DISPLAY_VIDEO_IDLE, .reason = DISPLAY_VIDEO_RECLAIM_NONE};
}

bool display_video_acquire(display_video_owner_t *o) {
    if (o->state != DISPLAY_VIDEO_IDLE) {
        o->refused++;
        return false;
    }
    o->state = DISPLAY_VIDEO_ACTIVE;
    o->reason = DISPLAY_VIDEO_RECLAIM_NONE;
    o->band_last = 0; // the bars are painted into both buffers at acquire
    o->acquires++;
    return true;
}

bool display_video_reclaim(display_video_owner_t *o, display_video_reclaim_t why) {
    if (o->state != DISPLAY_VIDEO_ACTIVE || why <= DISPLAY_VIDEO_RECLAIM_NONE || why >= DISPLAY_VIDEO_RECLAIM_COUNT) {
        o->refused++;
        return false;
    }
    o->state = DISPLAY_VIDEO_RECLAIMING;
    o->reason = why;
    o->reclaims++;
    return true;
}

bool display_video_reclaim_done(display_video_owner_t *o) {
    if (o->state != DISPLAY_VIDEO_RECLAIMING) {
        // IDLE is the benign race: the app released while the reclaim ran, so there is nothing left to finish
        if (o->state == DISPLAY_VIDEO_ACTIVE)
            o->refused++;
        return false;
    }
    o->state = DISPLAY_VIDEO_IDLE;
    o->band_last = 0;
    return true;
}

bool display_video_release(display_video_owner_t *o) {
    bool const was = o->state != DISPLAY_VIDEO_IDLE;
    o->state = DISPLAY_VIDEO_IDLE;
    o->reason = DISPLAY_VIDEO_RECLAIM_NONE;
    o->band_last = 0;
    return was;
}

char const *display_video_state_name(display_video_state_t s) {
    switch (s) {
    case DISPLAY_VIDEO_IDLE: return "idle";
    case DISPLAY_VIDEO_ACTIVE: return "active";
    case DISPLAY_VIDEO_RECLAIMING: return "reclaiming";
    }
    return "?";
}

char const *display_video_reclaim_name(display_video_reclaim_t r) {
    static char const *const names[DISPLAY_VIDEO_RECLAIM_COUNT] = {"none", "dialog", "thermal", "card", "escape",
                                                                   "engine"};
    return (unsigned)r < DISPLAY_VIDEO_RECLAIM_COUNT ? names[r] : "?";
}

display_video_plan_t display_video_plan(display_video_owner_t *o, uint32_t pending, bool present) {
    if (o->state != DISPLAY_VIDEO_ACTIVE)
        return (display_video_plan_t){.take = pending, .carry = 0, .swap = pending > 0, .video = false};
    if (!present)
        return (display_video_plan_t){0};
    display_video_plan_t const p = {.take = pending, .carry = o->band_last, .swap = true, .video = true};
    o->band_last = pending;
    return p;
}
