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

// Starts fn(arg) on a task of its own named name (cut to 15 characters, logged when cut), stack_bytes of stack. app
// NULL is the OS. handle (may be NULL) is set before the task first runs, as xTaskCreate does.
//
// fn ends by returning, never with vTaskDelete(NULL): the OS takes the task out of its book and deletes it after fn
// returns. A task that deletes itself stays in the book ("fw tasks" shows it as gone without returning).
// False when the book (TASK_BOOK_SLOTS) is full or the task could not be made (memory); nothing runs then.
bool lichen_task_start(lichen_app_t const *app, char const *name, void (*fn)(void *arg), void *arg,
                       uint32_t stack_bytes, lichen_prio_t prio, TaskHandle_t *handle);

// After an app stopped: its tasks still running, other than the caller's own. Waits up to wait_ms for them to end
// (a stopped task returns a moment after it said so), then logs each one left. Returns how many are left.
size_t lichen_task_leftovers(lichen_app_t const *app, uint32_t wait_ms);

// main.c, once: "fw tasks" (every task with its owner, priority, core and stack)
void lichen_task_console_register(void);

#ifdef __cplusplus
}
#endif
