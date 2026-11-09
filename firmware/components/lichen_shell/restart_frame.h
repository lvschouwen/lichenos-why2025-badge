#pragma once

// Where the "Restarting" frame (#39, #134) is drawn (#255). Pure (host test: restart_frame_test.c).
//
// lv_refr_now renders the whole screen on the calling task's stack: LVGL's draw path takes ~6 KiB (the LVGL task's
// own measurement, #26), so the 4 KiB console task (fw psram speed, fw reboot) overflowed it inside lv_draw_rect and
// a deliberate restart ended as a stack-guard panic and a crash record. A caller with less free stack only builds and
// loads the screen; the LVGL task (10 KiB, display.c) draws it on its next pass while the caller waits.

#include <stdbool.h>
#include <stddef.h>

#define RESTART_FRAME_STACK_NEED (6 * 1024)  // free stack lv_refr_now may use on the caller's task
#define RESTART_FRAME_HEAP_NEED  (64 * 1024) // largest PSRAM block: an out-of-memory restart must not allocate more

typedef enum {
    RESTART_FRAME_SKIP,    // no frame and no wait: the restart goes on without one
    RESTART_FRAME_HERE,    // build, load and draw it now on the caller's task (lv_refr_now)
    RESTART_FRAME_UI_TASK, // build and load it; the LVGL task draws it, the caller only waits
} restart_frame_t;

// psram_largest: the largest free PSRAM block (LVGL's heap); stack_free: bytes between the caller's stack pointer and
// the end of its stack; on_ui_task: the caller is the LVGL task. That one, short of stack, gets no frame: it is the task
// that would have to draw it, so loading the screen and waiting would only cost the wait.
restart_frame_t restart_frame_plan(size_t psram_largest, size_t stack_free, bool on_ui_task);
