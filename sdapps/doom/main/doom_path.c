// The pure half of the DOOM picture's path choice (#564), see doom_path.h.
#include "doom_path.h"

void doom_path_init(doom_path_t *p) {
    atomic_store(&p->state, DOOM_PATH_PENDING);
    atomic_store(&p->why, 0);
}

doom_path_state_t doom_path_get(doom_path_t *p) {
    return (doom_path_state_t)atomic_load_explicit(&p->state, memory_order_acquire);
}

void doom_path_chosen(doom_path_t *p, bool video_acquired) {
    doom_path_state_t s = doom_path_get(p);
    if (s != DOOM_PATH_PENDING && s != DOOM_PATH_PAUSED) {
        return;
    }
    atomic_store_explicit(&p->state, video_acquired ? DOOM_PATH_VIDEO : DOOM_PATH_CANVAS, memory_order_release);
}

bool doom_path_reclaimed(doom_path_t *p, int why) {
    if (doom_path_get(p) != DOOM_PATH_VIDEO) {
        return false;
    }
    atomic_store(&p->why, why);
    atomic_store_explicit(&p->state, DOOM_PATH_PAUSED, memory_order_release);
    return true;
}

int doom_path_why(doom_path_t *p) {
    return atomic_load(&p->why);
}

bool doom_path_own_back_ok(doom_path_t *p) {
    switch (doom_path_get(p)) {
    case DOOM_PATH_VIDEO:
        return false;
    case DOOM_PATH_PAUSED:
        return atomic_load(&p->why) != DOOM_RECLAIM_ESCAPE;
    default:
        return true; // loading or the canvas: no OS escape
    }
}

bool doom_path_engine_draws(doom_path_t *p) {
    doom_path_state_t s = doom_path_get(p);
    return s == DOOM_PATH_VIDEO || s == DOOM_PATH_CANVAS;
}
