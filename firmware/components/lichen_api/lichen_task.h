#pragma once
// Tasks for apps (#181, #84 T8a): an app never picks its own core or priority and never calls xTaskCreate*. It asks
// for a task with a priority class; the OS books it under the app's id, so "fw tasks" names the owner and an app that
// stops can check that nothing of it is left running.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_app.h"
#include "task_book.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The classes keep the numbers the tasks had before #181 (task_book.c, pinned by host_tests/task_book_test.c)
typedef enum {
    LICHEN_PRIO_BACKGROUND = TASK_CLASS_BACKGROUND, // priority 2, core 0: samplers, slow saves
    LICHEN_PRIO_SERVICE    = TASK_CLASS_SERVICE,    // 3, core 0: watchers, long scans
    LICHEN_PRIO_JOB        = TASK_CLASS_JOB,        // 4, core 0: a one-shot job a screen or a command waits for
    LICHEN_PRIO_URGENT     = TASK_CLASS_URGENT,     // 5, core 0: power off; not for an app's ordinary work
    LICHEN_PRIO_APP_LOOP   = TASK_CLASS_APP_LOOP,   // 3, core 1 with LVGL: an app's main loop (MeshCore's mesh task)
} lichen_prio_t;

// Where the task's stack lives (#269, task_book.h has the rules). INTERNAL is the default and right for nearly every
// task. LP_RAM: a low-rate task that must stay internal-capable (it writes NVS or /storage) but never puts a DMA
// buffer, PIE/SIMD data or a large flash/SD I/O buffer on its stack (esp_flash takes an LP RAM buffer as not-DRAM:
// reads go through a DMA bounce buffer that can fail with ESP_ERR_NO_MEM, writes go in 32 B pieces); it frees
// DMA-capable RAM.
// PSRAM: a task that never touches flash, NVS, /storage or OTA (the first one logs that rule as a warning).
// LP_RAM and PSRAM are for tasks that never return: deleting such a task itself needs a helper task in internal RAM
// and aborts when there is none, so a return is logged as an error and parks the task, suspended, for good.
// When the region is short the stack goes to internal RAM ("fw tasks" shows where each stack is).
typedef enum {
    LICHEN_STACK_INTERNAL = TASK_STACK_INTERNAL,
    LICHEN_STACK_LP_RAM   = TASK_STACK_LP_RAM,
    LICHEN_STACK_PSRAM    = TASK_STACK_PSRAM,
} lichen_stack_t;

// Starts fn(arg) on a task of its own named name (cut to 15 characters, logged when cut), stack_bytes of stack. ctx
// (#230 E1b) is who the task runs for: the task is booked under it and lichen_ctx_self() returns it on the task. An app
// passes the one lichen_register_app gave it; code that runs for whoever called it (the OS, a shell helper) passes
// lichen_ctx_self(), so a task an app's task or hook starts stays the app's. A stale or made-up ctx is refused (false,
// logged at most once per 10 s, counted in "fw app ctx"), never an abort. handle (may be NULL) is set before the task
// first runs, as xTaskCreate does. Any task, not an ISR.
//
// fn ends by returning, never with vTaskDelete(NULL): the OS takes the task out of its book and deletes it after fn
// returns. A task that deletes itself stays in the book ("fw tasks" shows it as gone without returning).
// False when the book (TASK_BOOK_SLOTS) is full or the task could not be made (memory); nothing runs then.
bool lichen_task_start(lichen_ctx_t ctx, char const *name, void (*fn)(void *arg), void *arg,
                       uint32_t stack_bytes, lichen_prio_t prio, TaskHandle_t *handle);

// lichen_task_start with the stack in mem (lichen_task_start is this with LICHEN_STACK_INTERNAL). Any task, not an ISR.
bool lichen_task_start_in(lichen_ctx_t ctx, char const *name, void (*fn)(void *arg), void *arg,
                          uint32_t stack_bytes, lichen_prio_t prio, lichen_stack_t mem, TaskHandle_t *handle);

// After an app stopped: ctx's tasks still running, other than the caller's own. Waits up to wait_ms for them to end
// (a stopped task returns a moment after it said so), then logs each one left. Returns how many are left (0 for a
// refused ctx). Blocks: not from the LVGL task, not an ISR.
size_t lichen_task_leftovers(lichen_ctx_t ctx, uint32_t wait_ms);

// main.c, once: "fw tasks" (every task with its owner, priority, core, stack and the memory the stack is in)
void lichen_task_console_register(void);

#ifdef __cplusplus
}
#endif
