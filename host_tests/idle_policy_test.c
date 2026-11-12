// Host test: the screen timeout's levels, the swallowed wake key and the Settings choices (#43)
#include "idle_policy.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

typedef struct {
    uint32_t     idle_s, dim_s, off_s;
    bool         inhibit;
    idle_level_t want;
} level_case_t;

static level_case_t const LEVELS[] = {
    // the defaults: dim at 60 s, off at 300 s
    {0, 60, 300, false, IDLE_BRIGHT},
    {59, 60, 300, false, IDLE_BRIGHT},
    {60, 60, 300, false, IDLE_DIM},
    {299, 60, 300, false, IDLE_DIM},
    {300, 60, 300, false, IDLE_OFF},
    {100000, 60, 300, false, IDLE_OFF},
    // inhibited (update, first-run setup, Bench): bright whatever the clock says
    {60, 60, 300, true, IDLE_BRIGHT},
    {300, 60, 300, true, IDLE_BRIGHT},
    // dim off: bright until off
    {299, 0, 300, false, IDLE_BRIGHT},
    {300, 0, 300, false, IDLE_OFF},
    // off off: dims and stays dim
    {100000, 60, 0, false, IDLE_DIM},
    // both off: never
    {100000, 0, 0, false, IDLE_BRIGHT},
    // off before dim: off wins, never dims
    {120, 300, 120, false, IDLE_OFF},
    {119, 300, 120, false, IDLE_BRIGHT},
    {60, 60, 60, false, IDLE_OFF},
};

int main(void) {
    for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++) {
        level_case_t const *c = &LEVELS[i];
        idle_level_t        got = idle_level(c->idle_s, (idle_times_t){c->dim_s, c->off_s}, c->inhibit);
        if (got != c->want) {
            printf("FAIL level case %zu: idle %u dim %u off %u inhibit %d: got %s, want %s\n", i, (unsigned)c->idle_s,
                   (unsigned)c->dim_s, (unsigned)c->off_s, c->inhibit, idle_level_name(got), idle_level_name(c->want));
            failures++;
        }
    }

    // Holds: busy restarts the clock whatever it says; setup holds for 10 min since the last key, then lets go
    CHECK(idle_hold(false, false, false, 0) == IDLE_HOLD_NONE);
    CHECK(idle_hold(true, false, false, 0) == IDLE_HOLD_RESTART);
    CHECK(idle_hold(true, false, true, 100000) == IDLE_HOLD_RESTART);
    CHECK(idle_hold(false, false, true, 0) == IDLE_HOLD_BRIGHT);
    CHECK(idle_hold(false, false, true, IDLE_SETUP_HOLD_S - 1) == IDLE_HOLD_BRIGHT);
    CHECK(idle_hold(false, false, true, IDLE_SETUP_HOLD_S) == IDLE_HOLD_NONE);
    // an abandoned setup goes straight to off with the default times
    CHECK(idle_level(IDLE_SETUP_HOLD_S, (idle_times_t){60, 300},
                     idle_hold(false, false, true, IDLE_SETUP_HOLD_S) != IDLE_HOLD_NONE) == IDLE_OFF);

    // An open test card (#355) holds bright like busy: a card opened on a dark or dimmed screen lights it, it never
    // dims or blanks while up, and the clock restarts, so the full timeout counts from when it closes
    CHECK(idle_hold(false, true, false, 0) == IDLE_HOLD_RESTART);
    CHECK(idle_hold(false, true, false, 100000) == IDLE_HOLD_RESTART);
    CHECK(idle_hold(false, true, true, IDLE_SETUP_HOLD_S) == IDLE_HOLD_RESTART);
    CHECK(idle_level(100000, (idle_times_t){60, 300}, idle_hold(false, true, false, 100000) != IDLE_HOLD_NONE) ==
          IDLE_BRIGHT);
    CHECK(idle_level(60, (idle_times_t){60, 300}, idle_hold(false, true, false, 60) != IDLE_HOLD_NONE) == IDLE_BRIGHT);
    // the key that finds the card up is not only a wake key: the held screen is bright, so it reaches the card
    CHECK(!idle_key_swallowed(idle_stage(idle_level(100000, (idle_times_t){60, 300},
                                                    idle_hold(false, true, false, 100000) != IDLE_HOLD_NONE),
                                         true, true, false)));
    // The pocket lock (lock on) engaged on the dark screen; a card opened over it lifts it, as the update screen does,
    // and the held bright stage does not engage it again, so the first Esc reaches the card, not the lock (#355)
    CHECK(idle_stage(IDLE_OFF, true, true, false).lock);
    CHECK(idle_lock_lifted(false, true));
    CHECK(idle_lock_lifted(true, false));
    CHECK(!idle_lock_lifted(false, false));
    idle_stage_t carded = idle_stage(idle_level(100000, (idle_times_t){60, 300},
                                                idle_hold(false, true, false, 100000) != IDLE_HOLD_NONE),
                                     true, true, false);
    CHECK(!carded.lock && !carded.saver && !idle_key_swallowed(carded));
    // closed again: the normal timer, counted from the restart
    CHECK(idle_hold(false, false, false, 0) == IDLE_HOLD_NONE);
    CHECK(idle_level(60, (idle_times_t){60, 300}, idle_hold(false, false, false, 60) != IDLE_HOLD_NONE) == IDLE_DIM);

    // Only a key that finds the screen off (or under the screensaver, #75) is swallowed; a dimmed screen's key acts
    CHECK(!idle_key_swallowed(idle_stage(IDLE_BRIGHT, false, false, false)));
    CHECK(!idle_key_swallowed(idle_stage(IDLE_DIM, false, false, false)));
    CHECK(idle_key_swallowed(idle_stage(IDLE_OFF, false, false, false)));
    CHECK(!idle_key_swallowed(idle_stage(IDLE_BRIGHT, true, true, false)));
    CHECK(idle_key_swallowed(idle_stage(IDLE_DIM, true, false, false)));
    CHECK(idle_key_swallowed(idle_stage(IDLE_OFF, true, false, false)));

    // The screensaver is the dim stage when it is on (#75); off is still off
    idle_stage_t st = idle_stage(IDLE_BRIGHT, true, true, false);
    CHECK(st.level == IDLE_BRIGHT && !st.saver && !st.lock);
    st = idle_stage(IDLE_DIM, true, false, false);
    CHECK(st.level == IDLE_DIM && st.saver && !st.lock);
    st = idle_stage(IDLE_DIM, false, false, false);
    CHECK(st.level == IDLE_DIM && !st.saver && !st.lock);
    st = idle_stage(IDLE_OFF, true, false, false);
    CHECK(st.level == IDLE_OFF && !st.saver && !st.lock);
    // The lock engages with the saver or the dark screen, never on a plain dimmed (readable) screen
    st = idle_stage(IDLE_DIM, true, true, false);
    CHECK(st.saver && st.lock);
    st = idle_stage(IDLE_DIM, false, true, false);
    CHECK(!st.saver && !st.lock);
    st = idle_stage(IDLE_OFF, false, true, false);
    CHECK(st.level == IDLE_OFF && st.lock);
    // "fw screensaver test": the saver now, at the dim backlight, even when the setting is off
    st = idle_stage(IDLE_BRIGHT, false, false, true);
    CHECK(st.level == IDLE_DIM && st.saver && !st.lock);
    st = idle_stage(IDLE_BRIGHT, false, true, true);
    CHECK(st.saver && st.lock);
    st = idle_stage(IDLE_OFF, false, false, true);
    CHECK(st.level == IDLE_OFF && !st.saver);

    // Stored tens of seconds
    idle_times_t t = idle_times_from_10s(IDLE_DIM_DEFAULT_10S, IDLE_OFF_DEFAULT_10S);
    CHECK(t.dim_s == 60 && t.off_s == 300);
    t = idle_times_from_10s(0, 180);
    CHECK(t.dim_s == 0 && t.off_s == 1800);

    // The rows cycle and wrap; an unknown value goes to the next one above it
    CHECK(idle_dim_next(0) == 3 && idle_dim_next(3) == 6 && idle_dim_next(6) == 12 && idle_dim_next(12) == 30);
    CHECK(idle_dim_next(30) == 0);
    CHECK(idle_dim_next(9) == 12 && idle_dim_next(200) == 0);
    CHECK(idle_off_next(0) == 12 && idle_off_next(12) == 30 && idle_off_next(30) == 60 && idle_off_next(60) == 180);
    CHECK(idle_off_next(180) == 0);
    CHECK(idle_off_next(20) == 30);

    char text[16];
    idle_choice_text(0, text, sizeof(text));
    CHECK(strcmp(text, "Off") == 0);
    idle_choice_text(3, text, sizeof(text));
    CHECK(strcmp(text, "30 s") == 0);
    idle_choice_text(6, text, sizeof(text));
    CHECK(strcmp(text, "1 min") == 0);
    idle_choice_text(180, text, sizeof(text));
    CHECK(strcmp(text, "30 min") == 0);
    idle_choice_text(9, text, sizeof(text));
    CHECK(strcmp(text, "90 s") == 0);

    CHECK(strcmp(idle_level_name(IDLE_DIM), "dim") == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("idle_policy_test ok\n");
    return 0;
}
