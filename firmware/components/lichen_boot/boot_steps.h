#pragma once

#include <stdbool.h>
#include <stdint.h>

// Which start-up step runs, is done, failed or was skipped (#66). Plain unlocked state, host-tested in
// host_tests/boot_steps_test.c; the badge calls the locked boot_progress.h wrappers instead.

typedef enum {
    BOOT_STORAGE,
    BOOT_DISPLAY,
    BOOT_PSRAM,
    BOOT_C6,
    BOOT_WIFI,
    BOOT_CLOCK,
    BOOT_LORA,
    BOOT_APPS, // every registered app's start_background (main.c); FAILED names the apps that did not start
    BOOT_STEP_COUNT
} boot_step_t;

typedef enum { STEP_PENDING, STEP_ACTIVE, STEP_DONE, STEP_FAILED, STEP_SKIPPED } step_state_t;

#define BOOT_REASON_LEN 48

typedef struct {
    step_state_t state[BOOT_STEP_COUNT];
    int          pct; // of the current step, -1 when it has none
    boot_step_t  current;
    char         reason[BOOT_STEP_COUNT][BOOT_REASON_LEN];
    bool         began[BOOT_STEP_COUNT];    // begin ran (a step can end skipped without it)
    bool         ended[BOOT_STEP_COUNT];
    uint32_t     begin_ms[BOOT_STEP_COUNT]; // uptime at begin / end (#39 measurement), see boot_steps_set_clock
    uint32_t     end_ms[BOOT_STEP_COUNT];
    uint32_t     generation; // grows on every change, so a screen can tell when to redraw
} boot_view_t;

// Where the begin/end uptimes come from (ms since reset); without one they stay 0. Set once, before the first step.
void        boot_steps_set_clock(uint32_t (*now_ms)(void));
void        boot_steps_reset(void);
void        boot_step_begin(boot_step_t step);
// A step that runs on its own while the boot goes on (the clock waits for SNTP): only its begin time is noted. Its state,
// the current step and pct stay as they are, so the boot screen does not wait for it or name it (#39)
void        boot_step_begin_background(boot_step_t step);
void        boot_step_progress(boot_step_t step, int pct);
// The first end after a begin counts; a later one is ignored until the step begins again (SNTP's hourly refresh ends
// the clock step again, #39)
void        boot_step_end(boot_step_t step, step_state_t state, char const *reason);
void        boot_steps_view(boot_view_t *out);
char const *boot_step_label(boot_step_t step);

// "fw boot fail <step>" saves step + 1; the next boot marks that step failed once. Only steps that end after NVS is up
// can be failed (not PSRAM): the flag is read from NVS
bool boot_step_failable(boot_step_t step);
int  boot_fail_step(uint8_t flag); // the step to fail, or -1
