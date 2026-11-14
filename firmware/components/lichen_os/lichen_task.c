// lichen_task_start (#181): every app task starts through a trampoline that books it under its owner and takes it out
// of the book when its function returns. The book (task_book.c) is guarded by a spinlock: the trampoline's last step
// runs right before vTaskDelete and must not block. A stack outside internal RAM (#269) is made WithCaps; such a task
// never returns (a return parks it, see park()).
#include "lichen_task.h"
#include "lichen_sys_app.h"
#include "lichen_sys_mem.h"
#include "gate_book.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/idf_additions.h"
#include "fw_console.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(configMAX_TASK_NAME_LEN == TASK_BOOK_NAME_CAP, "task_book.h's name cap must be FreeRTOS's");

static char const TAG[] = "task";

#define LEFTOVER_POLL_MS 20

// Internal .bss (~1.7 KB): a critical section must never touch PSRAM, whose cache can be off during a flash write
static task_book_t  book;
static portMUX_TYPE book_mux = portMUX_INITIALIZER_UNLOCKED;

// A task with its stack outside internal RAM returned, which lichen_task.h forbids: vTaskDeleteWithCaps(NULL) would
// need a clean-up task in internal RAM and aborts when there is none, so the task stays, suspended, in the book
// ("fw tasks" shows it, the app's stop check counts it) and its stack stays taken
static void park(int slot) {
    ESP_LOGE(TAG, "task %s returned but its stack is in %s: parked for good (such a task must never return)",
             book.slot[slot].name, task_stack_name((task_stack_t)book.slot[slot].mem));
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void book_take(void) {
    portENTER_CRITICAL(&book_mux);
}
static void book_give(void) {
    portEXIT_CRITICAL(&book_mux);
}
static gate_env_t const env = {.resolve = gate_os_resolve, .os = lichen_sys_ctx_os, .enter = lichen_ctx_enter,
                               .take = book_take, .give = book_give};

static void trampoline(void *arg) {
    int slot = (int)(intptr_t)arg;
    portENTER_CRITICAL(&book_mux);
    book.slot[slot].handle = xTaskGetCurrentTaskHandle();
    void (*fn)(void *)     = book.slot[slot].fn;
    void *fn_arg           = book.slot[slot].arg;
    bool  with_caps        = book.slot[slot].mem != TASK_STACK_INTERNAL;
    lichen_ctx_t ctx       = book.slot[slot].ctx;
    portEXIT_CRITICAL(&book_mux);
    gate_task_run(&env, ctx, fn, fn_arg); // runs as the context it was booked under (#230)
    if (with_caps) {
        park(slot);
    }
    portENTER_CRITICAL(&book_mux);
    task_book_remove(&book, slot);
    portEXIT_CRITICAL(&book_mux);
    vTaskDelete(NULL);
}

static uint32_t stack_caps(task_stack_t mem) {
    return (mem == TASK_STACK_LP_RAM ? MALLOC_CAP_RTCRAM : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT;
}

// Where the stack goes: the memory asked for while its region keeps its reserve (task_book.c), else internal RAM
static task_stack_t place_stack(task_stack_t mem, uint32_t stack_bytes) {
    if ((unsigned)mem >= TASK_STACK_COUNT || mem == TASK_STACK_INTERNAL) {
        return TASK_STACK_INTERNAL;
    }
    return task_stack_place(mem, stack_bytes, heap_caps_get_largest_free_block(stack_caps(mem)));
}

// Makes the booked task. A region that is short (place_stack) or cannot give the stack after all (a race with another
// allocation) leaves the task in internal RAM, logged once here; the slot learns that before the task can run, so the
// trampoline deletes it the plain way.
static bool create_task(int slot, task_stack_t asked, char const *name, uint32_t stack_bytes, int priority, int core,
                        TaskHandle_t *out) {
    void        *arg = (void *)(intptr_t)slot;
    task_stack_t mem = (task_stack_t)book.slot[slot].mem; // set by task_book_add, nothing else writes it yet
    if (mem != TASK_STACK_INTERNAL) {
        if (xTaskCreatePinnedToCoreWithCaps(trampoline, name, stack_bytes, arg, (UBaseType_t)priority, out, core,
                                            stack_caps(mem)) == pdPASS) {
            return true;
        }
        portENTER_CRITICAL(&book_mux);
        book.slot[slot].mem = TASK_STACK_INTERNAL;
        portEXIT_CRITICAL(&book_mux);
    }
    if (asked != TASK_STACK_INTERNAL) {
        ESP_LOGW(TAG, "task %s: %s is short, its %u B stack goes to internal RAM", name, task_stack_name(asked),
                 (unsigned)stack_bytes);
    }
    return xTaskCreatePinnedToCore(trampoline, name, stack_bytes, arg, (UBaseType_t)priority, out, core) == pdPASS;
}

// The first PSRAM stack names audit D's rule in the log, so a task that breaks it is easy to find after the assert
static void warn_psram_once(char const *owner, char const *name) {
    static bool warned;
    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "%s: task %s has its stack in PSRAM: no NVS, /storage, flash or OTA from it (assert with the "
                      "cache off)", owner, name);
    }
}

bool lichen_task_start(lichen_ctx_t ctx, char const *name, void (*fn)(void *arg), void *arg,
                       uint32_t stack_bytes, lichen_prio_t prio, TaskHandle_t *handle) {
    return lichen_task_start_in(ctx, name, fn, arg, stack_bytes, prio, LICHEN_STACK_INTERNAL, handle);
}

bool lichen_task_start_in(lichen_ctx_t ctx, char const *name, void (*fn)(void *arg), void *arg,
                          uint32_t stack_bytes, lichen_prio_t prio, lichen_stack_t mem, TaskHandle_t *handle) {
    // #229: a task runs off screen, so it is BACKGROUND; the OS's and the shell's (SYSTEM) tasks always pass
    if (!lichen_sys_caps_ok(ctx, CAP_BACKGROUND, "lichen_task_start")) {
        return false;
    }
    int priority = task_class_priority((task_class_t)prio);
    if (!fn || priority < 0) {
        ESP_LOGE(TAG, "task %s not started: no function or an unknown class %d", name ? name : "?", (int)prio);
        return false;
    }
    char         fitted[TASK_BOOK_NAME_CAP];
    bool         cut    = task_name_fit(fitted, name);
    task_stack_t asked  = (unsigned)mem < TASK_STACK_COUNT ? (task_stack_t)mem : TASK_STACK_INTERNAL;
    task_stack_t placed = place_stack(asked, stack_bytes); // the heap query stays outside the spinlock
    task_spec_t  spec   = {.name = fitted, .stack = stack_bytes, .cls = (task_class_t)prio, .mem = placed, .fn = fn,
                           .arg = arg};
    gate_owner_t who;
    int          slot = gate_task_add(&env, &book, ctx, &spec, &who);
    if (slot == TASK_GATE_REFUSED) {
        return false; // logged and counted by the context table
    }
    char const *owner = who.label ? who.label : TASK_BOOK_OS;
    if (cut) {
        ESP_LOGW(TAG, "%s: task name \"%s\" cut to \"%s\"", owner, name, fitted);
    }
    if (asked == TASK_STACK_PSRAM) {
        warn_psram_once(owner, fitted);
    }
    if (slot < 0) {
        ESP_LOGE(TAG, "%s: task %s not started: all %d task slots are taken", owner, fitted, TASK_BOOK_SLOTS);
        return false;
    }
    // Straight into the caller's variable: FreeRTOS writes it before the task can run (mesh_task_handle relies on it)
    TaskHandle_t own  = NULL;
    int          core = task_class_core((task_class_t)prio);
    if (!create_task(slot, asked, fitted, stack_bytes, priority, core, handle ? handle : &own)) {
        portENTER_CRITICAL(&book_mux);
        task_book_remove(&book, slot);
        portEXIT_CRITICAL(&book_mux);
        ESP_LOGE(TAG, "%s: task %s not started: no memory for %u B of stack", owner, fitted, (unsigned)stack_bytes);
        return false;
    }
    return true;
}

size_t lichen_task_leftovers(lichen_ctx_t ctx, uint32_t wait_ms) {
    gate_owner_t who;
    if (!gate_owner(&env, ctx, "lichen_task_leftovers", &who)) {
        return 0;
    }
    char const  *owner = who.label ? who.label : TASK_BOOK_OS;
    TaskHandle_t self  = xTaskGetCurrentTaskHandle();
    size_t       left  = 0;
    for (uint32_t waited = 0;; waited += LEFTOVER_POLL_MS) {
        portENTER_CRITICAL(&book_mux);
        left = task_book_count(&book, ctx, self);
        portEXIT_CRITICAL(&book_mux);
        if (!left || waited >= wait_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(LEFTOVER_POLL_MS));
    }
    for (int i = 0; left && i < TASK_BOOK_SLOTS; i++) {
        char name[TASK_BOOK_NAME_CAP] = "";
        portENTER_CRITICAL(&book_mux);
        task_slot_t const *s = &book.slot[i];
        if (s->used && s->ctx == ctx && s->handle != self) {
            memcpy(name, s->name, sizeof(name));
        }
        portEXIT_CRITICAL(&book_mux);
        if (name[0]) {
            ESP_LOGW(TAG, "%s stopped, but its task %s still runs", owner, name);
        }
    }
    return left;
}

// ---- fw tasks ------------------------------------------------------------------------------------------------------

static task_slot_t const *slot_of(task_book_t const *snap, TaskHandle_t h) {
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        if (snap->slot[i].used && snap->slot[i].handle == h) {
            return &snap->slot[i];
        }
    }
    return NULL;
}

static bool alive(TaskStatus_t const *tasks, UBaseType_t n, void const *h) {
    for (UBaseType_t i = 0; i < n; i++) {
        if (tasks[i].xHandle == h) {
            return true;
        }
    }
    return false;
}

// From the stack's address, so it holds for every task, booked or not, and shows a fallback to internal RAM
static char const *stack_mem_name(void const *stack_base) {
    if (esp_ptr_external_ram(stack_base)) {
        return task_stack_name(TASK_STACK_PSRAM);
    }
    if (esp_ptr_in_rtc_dram_fast(stack_base)) {
        return task_stack_name(TASK_STACK_LP_RAM);
    }
    return task_stack_name(TASK_STACK_INTERNAL);
}

static void print_tasks(task_book_t const *snap, TaskStatus_t const *tasks, UBaseType_t n) {
    for (UBaseType_t i = 0; i < n; i++) {
        task_slot_t const *s = slot_of(snap, tasks[i].xHandle);
        char               core[4];
        if (tasks[i].xCoreID == tskNO_AFFINITY) {
            strcpy(core, "-");
        } else {
            snprintf(core, sizeof(core), "%u", (unsigned)tasks[i].xCoreID & 1u);
        }
        char stack[12] = "?";
        if (s) {
            snprintf(stack, sizeof(stack), "%u", (unsigned)s->stack);
        }
        console_reply("task %s owner %s prio %u core %s stack %s free %u mem %s", tasks[i].pcTaskName,
                      s ? s->owner : "-", (unsigned)tasks[i].uxCurrentPriority, core, stack,
                      (unsigned)tasks[i].usStackHighWaterMark, stack_mem_name(tasks[i].pxStackBase));
    }
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        task_slot_t const *s = &snap->slot[i];
        if (s->used && s->handle && !alive(tasks, n, s->handle)) {
            // Almost always a task that deleted itself instead of returning; a task that ends or starts while this
            // runs can show here once
            console_reply("task %s owner %s: booked but not in the task list (vTaskDelete in the task? its slot stays "
                          "taken)", s->name, s->owner);
        }
    }
}

// "fw tasks": owner "-" is a task the OS made itself without the book (drivers, IDF, services)
static void cmd_tasks(char const *args) {
    (void)args;
    UBaseType_t   n     = uxTaskGetNumberOfTasks() + 4; // room for tasks started meanwhile
    TaskStatus_t *tasks = heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    task_book_t  *snap  = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM); // off the console's 4 KB stack
    if (!tasks || !snap) {
        console_reply("tasks: out of memory");
        heap_caps_free(tasks);
        heap_caps_free(snap);
        return;
    }
    n = uxTaskGetSystemState(tasks, n, NULL);
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        task_slot_t slot; // one slot at a time onto the stack: short critical sections, the PSRAM copy outside them
        portENTER_CRITICAL(&book_mux);
        slot = book.slot[i];
        portEXIT_CRITICAL(&book_mux);
        snap->slot[i] = slot;
    }
    print_tasks(snap, tasks, n);
    size_t booked = 0;
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        booked += snap->slot[i].used;
    }
    console_reply("tasks: %u running, %u of %d task slots taken", (unsigned)n, (unsigned)booked, TASK_BOOK_SLOTS);
    console_reply("tasks: LP RAM %u of %u B free, largest block %u B (stacks there keep %u B free)",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM),
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_RTCRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM),
                  (unsigned)task_stack_reserve(TASK_STACK_LP_RAM));
    heap_caps_free(tasks);
    heap_caps_free(snap);
}

// lichen_sys_mem.h (#278): the size comes from the book, so only booked tasks have one
static uint32_t booked_size(TaskHandle_t h) {
    uint32_t size = 0;
    portENTER_CRITICAL(&book_mux);
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        if (book.slot[i].used && book.slot[i].handle == h) {
            size = book.slot[i].stack;
        }
    }
    portEXIT_CRITICAL(&book_mux);
    return size;
}

size_t lichen_sys_mem_stacks(lichen_sys_stack_t *out, size_t cap) {
    UBaseType_t   n     = uxTaskGetNumberOfTasks() + 4; // room for tasks started meanwhile
    TaskStatus_t *tasks = heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (!tasks) {
        return 0;
    }
    n = uxTaskGetSystemState(tasks, n, NULL);
    for (UBaseType_t i = 0; i < n && i < cap; i++) {
        strlcpy(out[i].name, tasks[i].pcTaskName, sizeof(out[i].name));
        out[i].free = tasks[i].usStackHighWaterMark;
        out[i].size = booked_size(tasks[i].xHandle);
    }
    heap_caps_free(tasks);
    return n;
}

void lichen_task_console_register(void) {
    console_register("tasks", "tasks (every task with its owner app, priority, core, stack size, bytes never used and "
                              "the memory its stack is in; LP RAM free)",
                     cmd_tasks);
}
