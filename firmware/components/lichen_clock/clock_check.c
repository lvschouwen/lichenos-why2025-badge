#include "clock_check.h"

#include <stdio.h>

bool clock_in_window(int64_t t, int64_t build_epoch) {
    return t >= build_epoch - CLOCK_BELOW_BUILD_S && t <= build_epoch + CLOCK_ABOVE_BUILD_S;
}

clock_verdict_t clock_check(int64_t cand, int64_t now, int64_t build_epoch, bool synced) {
    if (cand < build_epoch - CLOCK_BELOW_BUILD_S) {
        return CLOCK_TOO_OLD;
    }
    if (cand > build_epoch + CLOCK_ABOVE_BUILD_S) {
        return CLOCK_TOO_NEW;
    }
    if (synced) {
        int64_t step = cand - now;
        if (step > CLOCK_MAX_STEP_S || step < -CLOCK_MAX_STEP_S) {
            return CLOCK_BIG_STEP;
        }
    }
    return CLOCK_OK;
}

clock_verdict_t clock_check_guarded(clock_guard_t *g, int64_t cand, int64_t now, int64_t build_epoch, bool synced,
                                    int64_t mono_s) {
    clock_verdict_t v = clock_check(cand, now, build_epoch, synced);
    if (v != CLOCK_BIG_STEP) {
        g->count = 0;
        return v;
    }
    int64_t off  = cand - now;
    int64_t diff = off - g->offset;
    if (g->count > 0 && diff <= CLOCK_RECOVER_AGREE_S && diff >= -CLOCK_RECOVER_AGREE_S) {
        if (mono_s - g->at_s < CLOCK_RECOVER_SPACE_S) {
            return CLOCK_BIG_STEP; // too soon after the last counted answer: not counted
        }
        g->count++;
    } else {
        g->count = 1;
    }
    g->offset = off;
    g->at_s   = mono_s;
    if (g->count >= CLOCK_RECOVER_ANSWERS) {
        g->count = 0;
        return CLOCK_RECOVERED;
    }
    return CLOCK_BIG_STEP;
}

clock_verdict_t clock_decide(clock_src_t cur, clock_src_t src, clock_guard_t *g, int64_t cand, int64_t now,
                             int64_t build_epoch, int64_t mono_s) {
    if (src < CLOCK_SRC_APP || src > CLOCK_SRC_SNTP) {
        return CLOCK_NO_SOURCE;
    }
    if (src < cur) {
        return CLOCK_OUTRANKED;
    }
    if (src == CLOCK_SRC_SNTP) {
        return clock_check_guarded(g, cand, now, build_epoch, cur == CLOCK_SRC_SNTP, mono_s);
    }
    clock_verdict_t v = clock_check(cand, now, build_epoch, false);
    if (v != CLOCK_OK || src == CLOCK_SRC_MANUAL) {
        return v;
    }
    return clock_in_window(now, build_epoch) && cand <= now ? CLOCK_NOT_FORWARD : CLOCK_OK;
}

char const *clock_verdict_text(clock_verdict_t v) {
    switch (v) {
        case CLOCK_OK: return "ok";
        case CLOCK_TOO_OLD: return "older than this firmware";
        case CLOCK_TOO_NEW: return "too far in the future";
        case CLOCK_BIG_STEP: return "jump too big after a sync";
        case CLOCK_RECOVERED: return "big step confirmed by repeated answers";
        case CLOCK_OUTRANKED: return "a better source set the clock";
        case CLOCK_NOT_FORWARD: return "would move the clock backwards";
        case CLOCK_NO_SOURCE: return "not a source that sets the clock";
    }
    return "?";
}

char const *clock_src_text(clock_src_t s) {
    switch (s) {
        case CLOCK_SRC_NONE: return "not set";
        case CLOCK_SRC_RTC: return "carried over the restart";
        case CLOCK_SRC_APP: return "from mesh contacts, unverified";
        case CLOCK_SRC_MANUAL: return "set by hand";
        case CLOCK_SRC_SNTP: return "SNTP";
    }
    return "?";
}

// "just now", "3 min ago", "2 h ago", "4 d ago"
static void age_text(int64_t age_s, char *out, size_t cap) {
    if (age_s < 60) {
        snprintf(out, cap, "just now");
    } else if (age_s < 3600) {
        snprintf(out, cap, "%lld min ago", (long long)(age_s / 60));
    } else if (age_s < 86400) {
        snprintf(out, cap, "%lld h ago", (long long)(age_s / 3600));
    } else {
        snprintf(out, cap, "%lld d ago", (long long)(age_s / 86400));
    }
}

// Short enough for "carried over the restart, was ..." to fit About's and fw clock's 48-byte buffers
static char const *was_text(clock_src_t s) {
    switch (s) {
        case CLOCK_SRC_SNTP: return "SNTP";
        case CLOCK_SRC_MANUAL: return "set by hand";
        case CLOCK_SRC_APP: return "from mesh";
        default: return NULL;
    }
}

void clock_src_line(clock_src_t s, clock_src_t was, int64_t age_s, char *out, size_t cap) {
    char age[24];
    if (s == CLOCK_SRC_SNTP || s == CLOCK_SRC_MANUAL) {
        age_text(age_s, age, sizeof(age));
        snprintf(out, cap, "%s, %s", clock_src_text(s), age);
    } else if (s == CLOCK_SRC_RTC && was_text(was)) {
        snprintf(out, cap, "%s, was %s", clock_src_text(s), was_text(was));
    } else {
        snprintf(out, cap, "%s", clock_src_text(s));
    }
}

char const *clock_src_short(clock_src_t s) {
    switch (s) {
        case CLOCK_SRC_NONE: return "not set";
        case CLOCK_SRC_RTC: return "carried";
        case CLOCK_SRC_APP: return "mesh";
        case CLOCK_SRC_MANUAL: return "by hand";
        case CLOCK_SRC_SNTP: return "SNTP";
    }
    return "?";
}

clock_src_t clock_src_was(clock_src_t cur, clock_src_t stored) {
    if (cur != CLOCK_SRC_RTC || stored < CLOCK_SRC_APP || stored > CLOCK_SRC_SNTP) {
        return CLOCK_SRC_NONE;
    }
    return stored;
}

bool clock_src_unverified(clock_src_t cur, clock_src_t was) {
    return cur == CLOCK_SRC_APP || (cur == CLOCK_SRC_RTC && was == CLOCK_SRC_APP);
}

clock_src_t clock_force_rank(clock_src_t cur) {
    return cur > CLOCK_SRC_MANUAL ? CLOCK_SRC_MANUAL : cur;
}
