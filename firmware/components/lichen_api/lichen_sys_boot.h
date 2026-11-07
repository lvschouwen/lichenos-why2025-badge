#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The boot progress and the console log tap, the shell's side (#244, #216 decision 1): the boot screen and the log
// store read them through here, never from lichen_boot headers. System surface: tools/layers.conf forbids lichen_sys_*
// in L6. The types repeat lichen_boot's (the gate cannot include it); lichen_sys_pure_view.c keeps them equal with
// asserts. Everything is safe from any task unless it says otherwise.

typedef enum {
    LICHEN_BOOT_STORAGE,
    LICHEN_BOOT_DISPLAY,
    LICHEN_BOOT_PSRAM,
    LICHEN_BOOT_C6,
    LICHEN_BOOT_WIFI,
    LICHEN_BOOT_CLOCK,
    LICHEN_BOOT_LORA,
    LICHEN_BOOT_APPS,
    LICHEN_BOOT_STEP_COUNT
} lichen_boot_step_t;

typedef enum {
    LICHEN_STEP_PENDING,
    LICHEN_STEP_ACTIVE,
    LICHEN_STEP_DONE,
    LICHEN_STEP_FAILED,
    LICHEN_STEP_SKIPPED
} lichen_step_state_t;

#define LICHEN_BOOT_REASON_LEN 48

typedef struct {
    lichen_step_state_t state[LICHEN_BOOT_STEP_COUNT];
    int                 pct; // of the current step, -1 when it has none
    lichen_boot_step_t  current;
    char                reason[LICHEN_BOOT_STEP_COUNT][LICHEN_BOOT_REASON_LEN];
    bool                began[LICHEN_BOOT_STEP_COUNT];
    bool                ended[LICHEN_BOOT_STEP_COUNT];
    uint32_t            begin_ms[LICHEN_BOOT_STEP_COUNT];
    uint32_t            end_ms[LICHEN_BOOT_STEP_COUNT];
    uint32_t            generation; // grows on every change, so a screen can tell when to redraw
} lichen_boot_view_t;

#define LICHEN_LOG_TAP_LINES    8
#define LICHEN_LOG_TAP_LINE_LEN 96

// A copy of the boot progress. Any task.
void        lichen_sys_boot_view(lichen_boot_view_t *out);
// The step's short name ("storage"). Static text, pure, any task.
char const *lichen_sys_boot_label(lichen_boot_step_t step);
// The one human line for a step, written into buf (terminated, cut if cap is small); pct counts only for the C6 image
// update, pass -1 otherwise. Pure, any task.
char const *lichen_sys_boot_sentence(lichen_boot_step_t step, int pct, char *buf, size_t cap);
// Steps done, failed or skipped over all steps, 0..100. Pure, any task.
int         lichen_sys_boot_overall_pct(lichen_boot_view_t const *v);
// Up to max of the last console lines, newest last, ANSI colour codes stripped; returns how many. Any task.
size_t      lichen_sys_log_tap_last(char lines[][LICHEN_LOG_TAP_LINE_LEN], size_t max);
// Drops ESC [ ... colour sequences and every CR and LF; out holds len + 1 bytes, in and out may be the same buffer;
// returns the new length. Pure, any task.
size_t      lichen_sys_log_clean(char const *in, size_t len, char *out);
