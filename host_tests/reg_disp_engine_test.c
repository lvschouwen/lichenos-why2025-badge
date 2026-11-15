// Host test for disp.engine's set path (#468 review): the real gate of reg_disp_engine.c against stubbed developer
// mode, UI lock and display. A release image and developer mode off refuse without touching the lock or the display;
// a bad id is refused before the lock; a set takes and gives back the lock exactly once and applies.
#include "display_engine.h"
#include "display_info.h"
#include "lichen_sys_ui.h"
#include "reg_disp_engine.h"

#include <stdio.h>

#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool     dev_on, lock_free = true, display_says_yes = true;
static int      locks, unlocks, sets;
static uint32_t applied;

bool console_dev_mode(void) {
    return dev_on;
}

bool lichen_sys_ui_lock(uint32_t timeout_ms) {
    CHECK(timeout_ms == REG_DISP_ENGINE_LOCK_MS);
    if (!lock_free) {
        return false;
    }
    locks++;
    return true;
}

void lichen_sys_ui_unlock(void) {
    unlocks++;
}

bool display_engine_set(uint32_t id) {
    CHECK(locks == unlocks + 1); // only under the lock
    sets++;
    if (display_says_yes) {
        applied = id;
    }
    return display_says_yes;
}

static void reset(void) {
    dev_on = false, lock_free = true, display_says_yes = true;
    locks = unlocks = sets = 0;
    applied                = DISPLAY_ENGINE_CPU;
}

static void test_release_refuses(void) {
    reset();
    dev_on = true;
    CHECK(reg_disp_engine_set(DISPLAY_ENGINE_PPA, false) == REG_DISP_ENGINE_RELEASE);
    CHECK(locks == 0 && sets == 0 && applied == DISPLAY_ENGINE_CPU);
}

static void test_dev_off_refuses(void) {
    reset();
    CHECK(reg_disp_engine_set(DISPLAY_ENGINE_PPA_ASYNC, true) == REG_DISP_ENGINE_DEV_OFF);
    CHECK(locks == 0 && sets == 0 && applied == DISPLAY_ENGINE_CPU);
    CHECK(strcmp(reg_disp_engine_why_text(REG_DISP_ENGINE_DEV_OFF), "developer mode is off") == 0);
}

static void test_dev_on_applies(void) {
    reset();
    dev_on = true;
    CHECK(reg_disp_engine_set(DISPLAY_ENGINE_PPA_ASYNC, true) == REG_DISP_ENGINE_SET);
    CHECK(applied == DISPLAY_ENGINE_PPA_ASYNC && sets == 1 && locks == 1 && unlocks == 1);
    CHECK(reg_disp_engine_set(DISPLAY_ENGINE_CPU, true) == REG_DISP_ENGINE_SET);
    CHECK(applied == DISPLAY_ENGINE_CPU && locks == 2 && unlocks == 2);
}

static void test_bad_value(void) {
    reset();
    dev_on = true;
    CHECK(reg_disp_engine_set(DISPLAY_ENGINE_COUNT, true) == REG_DISP_ENGINE_BAD);
    CHECK(reg_disp_engine_set(UINT32_MAX, true) == REG_DISP_ENGINE_BAD);
    CHECK(locks == 0 && sets == 0);
}

static void test_lock_and_display_refusals(void) {
    reset();
    dev_on    = true;
    lock_free = false;
    CHECK(reg_disp_engine_set(DISPLAY_ENGINE_PPA, true) == REG_DISP_ENGINE_NO_LOCK);
    CHECK(sets == 0 && unlocks == 0);
    lock_free        = true;
    display_says_yes = false; // the PPA still busy after a timeout
    CHECK(reg_disp_engine_set(DISPLAY_ENGINE_PPA, true) == REG_DISP_ENGINE_REFUSED);
    CHECK(locks == 1 && unlocks == 1 && applied == DISPLAY_ENGINE_CPU);
}

int main(void) {
    test_release_refuses();
    test_dev_off_refuses();
    test_dev_on_applies();
    test_bad_value();
    test_lock_and_display_refusals();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("reg_disp_engine_test: all passed\n");
    return 0;
}
