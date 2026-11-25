#include "booked_sweep.h"

#include <stdio.h>

bool booked_sweep_line(char *buf, size_t cap, char const *id, uint32_t blocks, uint32_t bytes, bool unsafe) {
    if (blocks == 0 && !unsafe) {
        return false;
    }
    snprintf(buf, cap, "%s: freed %u booked blocks (%u B)%s", id, (unsigned)blocks, (unsigned)bytes,
             unsafe ? ", not freed: untracked frees" : "");
    return true;
}

void booked_release(booked_release_env_t const *env, void *p, bool stale) {
    env->forget_blocks(env->user, p);
    env->forget_booked(env->user, p);
    if (stale) {
        env->free_stale(env->user, p);
    } else {
        env->free_owned(env->user, p);
    }
}
