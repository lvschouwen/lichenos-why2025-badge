#pragma once

// Where a full-screen draw (lv_refr_now) may run (#259, after #255). Pure (host test: draw_stack_test.c).
//
// lv_refr_now renders on the calling task's stack, and LVGL's draw path takes ~6 KiB (the LVGL task's own measurement,
// #26). The 4 KiB console task overflowed it (#255, the restart frame); "fw ui perf bench" and "fw bench" redrew the
// same way. A caller with less free stack hands the draw to the LVGL task (10 KiB, display.c) and waits for it.

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define DRAW_STACK_NEED (6 * 1024) // free stack lv_refr_now may use on the caller's task

typedef enum {
    DRAW_HERE,    // enough free stack: draw on the caller's task
    DRAW_UI_TASK, // hand it to the LVGL task and wait for it
    DRAW_REFUSE,  // nowhere safe: the caller is the LVGL task itself, or that task is not known yet (before shell_start)
} draw_stack_t;

// stack_free: bytes between the caller's stack pointer and the end of its stack; on_ui_task: the caller is the LVGL
// task; ui_task_known: the shell has noted the LVGL task, so a handed-over draw would run on a known 10 KiB stack
draw_stack_t draw_stack_plan(size_t stack_free, bool on_ui_task, bool ui_task_known);

// The caller's wait for a draw handed to the LVGL task (#259). Its wake-ups share the task's one notification slot with
// ui_probe's waits, whose late gives (after their own timeout) can wake it early: so it waits until the job's done flag,
// not for one wake-up, or it returns while the job still runs on its stack frame. The job gives first and sets done last
// (its last touch of the frame), so wait is a timed wait (a give taken before done showed up is not waited for again);
// clear then drops the job's give, if still pending, so it cannot end a later wait (ui_probe's) early.
typedef struct {
    void (*wait)(void *ctx);  // one timed wait for a wake-up
    void (*clear)(void *ctx); // drop pending wake-ups
    void *ctx;
} draw_wait_ops_t;

void draw_wait_done(atomic_bool const *done, draw_wait_ops_t const *ops);
