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
    atomic_bool  done;
} draw_job_t;

#define DRAW_WAIT_TICK_MS 10 // only after a give taken before done showed up; a normal wait ends on the job's give

// On the LVGL task. Gives, then sets done: after done the caller may return and the frame is gone, so done is last
static void draw_job(void *arg) {
    draw_job_t  *job    = arg;
    TaskHandle_t waiter = job->waiter;
    job->fn(job->arg);
    xTaskNotifyGive(waiter);
    atomic_store_explicit(&job->done, true, memory_order_release);
}

static void wait_tick(void *unused) {
    (void)unused;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DRAW_WAIT_TICK_MS));
}

static void wait_clear(void *unused) {
    (void)unused;
    ulTaskNotifyTake(pdTRUE, 0);
}

// Handed to the LVGL task. Waits without a timeout, as lichen_sys_ui_lock(0) did before: the job is on this stack
static draw_call_t draw_on_ui_task(void (*fn)(void *), void *arg) {
    static draw_wait_ops_t const ops = {.wait = wait_tick, .clear = wait_clear};
    draw_job_t job = {.fn = fn, .arg = arg, .waiter = xTaskGetCurrentTaskHandle()};
    atomic_init(&job.done, false);
    if (!lichen_sys_ui_call(draw_job, &job, 0)) {
        return DRAW_CALL_NO_UI;
    }
    draw_wait_done(&job.done, &ops);
    return DRAW_CALL_RAN;
}

draw_call_t shell_draw_call(void (*fn)(void *), void *arg) {
    if (!lichen_sys_ui_lock(0)) {
        return DRAW_CALL_NO_UI; // no display: the UI service refuses the lock (#248)
    }
    bool         known = atomic_load_explicit(&ui_task, memory_order_relaxed) != 0;
    draw_stack_t where = draw_stack_plan(shell_stack_free_here(), shell_on_ui_task(), known);
    if (where == DRAW_HERE) {
        fn(arg);
    }
    lichen_sys_ui_unlock(); // before a hand-over: the LVGL task needs the lock to run the job
    if (where == DRAW_UI_TASK) {
        return draw_on_ui_task(fn, arg);
    }
    return where == DRAW_HERE ? DRAW_CALL_RAN : DRAW_CALL_NO_TASK;
}
