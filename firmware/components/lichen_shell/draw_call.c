// Full-screen draws on a stack big enough for them (#255, #259): the LVGL task's handle, the caller's free stack, and
// shell_draw_call, which runs a draw here or hands it to the LVGL task and waits (draw_stack.h says which).
#include "shell_parts.h"

#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_sys_ui.h"

#include <stdatomic.h>
#include <stdint.h>

// The LVGL task's handle, noted on its first pass after shell_start (0 until then)
static _Atomic uintptr_t ui_task;

static void note_ui_task(void *unused) {
    (void)unused;
    atomic_store_explicit(&ui_task, (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_relaxed);
}

bool shell_note_ui_task(void) {
    return lichen_sys_ui_call(note_ui_task, NULL, 0);
}

bool shell_on_ui_task(void) {
    return atomic_load_explicit(&ui_task, memory_order_relaxed) == (uintptr_t)xTaskGetCurrentTaskHandle();
}

// Free bytes below the caller's stack pointer: the stack grows down towards xTaskGetStackStart
size_t shell_stack_free_here(void) {
    uintptr_t sp    = (uintptr_t)esp_cpu_get_sp();
    uintptr_t start = (uintptr_t)xTaskGetStackStart(NULL);
    return sp > start ? (size_t)(sp - start) : 0;
}

typedef struct {
    void (*fn)(void *);
    void        *arg;
    TaskHandle_t waiter;
} draw_job_t;

static void draw_job(void *arg) {
    draw_job_t *job = arg;
    job->fn(job->arg);
    xTaskNotifyGive(job->waiter);
}

draw_stack_t shell_draw_call(void (*fn)(void *), void *arg) {
    bool         known = atomic_load_explicit(&ui_task, memory_order_relaxed) != 0;
    draw_stack_t where = draw_stack_plan(shell_stack_free_here(), shell_on_ui_task(), known);
    if (where == DRAW_HERE) {
        lichen_sys_ui_lock(0);
        fn(arg);
        lichen_sys_ui_unlock();
    } else if (where == DRAW_UI_TASK) {
        // Waits without a timeout, as lichen_sys_ui_lock(0) did before: the job lives on this stack until it has run
        draw_job_t job = {.fn = fn, .arg = arg, .waiter = xTaskGetCurrentTaskHandle()};
        ulTaskNotifyTake(pdTRUE, 0); // a stale notification (ui_probe's settle) must not end the wait early
        if (!lichen_sys_ui_call(draw_job, &job, 0)) {
            return DRAW_REFUSE;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    return where;
}
