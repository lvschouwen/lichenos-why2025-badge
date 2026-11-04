// lichen_task_start (#181): every app task starts through a trampoline that books it under its owner and takes it out
// of the book when its function returns. The book (task_book.c) is guarded by a spinlock: the trampoline's last step
// runs right before vTaskDelete and must not block.
#include "lichen_task.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fw_console.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(configMAX_TASK_NAME_LEN == TASK_BOOK_NAME_CAP, "task_book.h's name cap must be FreeRTOS's");

static char const TAG[] = "task";

#define LEFTOVER_POLL_MS 20

EXT_RAM_BSS_ATTR static task_book_t book; // ~2 KB: PSRAM, internal RAM is the scarce one
static portMUX_TYPE                 book_mux = portMUX_INITIALIZER_UNLOCKED;

static void trampoline(void *arg) {
    int slot = (int)(intptr_t)arg;
    portENTER_CRITICAL(&book_mux);
    book.slot[slot].handle = xTaskGetCurrentTaskHandle();
    void (*fn)(void *)     = book.slot[slot].fn;
    void *fn_arg           = book.slot[slot].arg;
    portEXIT_CRITICAL(&book_mux);
    fn(fn_arg);
    portENTER_CRITICAL(&book_mux);
    task_book_remove(&book, slot);
    portEXIT_CRITICAL(&book_mux);
    vTaskDelete(NULL);
}

bool lichen_task_start(lichen_app_t const *app, char const *name, void (*fn)(void *arg), void *arg,
                       uint32_t stack_bytes, lichen_prio_t prio, TaskHandle_t *handle) {
    char const *owner = app ? app->id : TASK_BOOK_OS;
    int         priority = task_class_priority((task_class_t)prio);
    if (!fn || priority < 0) {
        ESP_LOGE(TAG, "%s: task %s not started: no function or an unknown class %d", owner, name ? name : "?", (int)prio);
        return false;
    }
    char fitted[TASK_BOOK_NAME_CAP];
    if (task_name_fit(fitted, name)) {
        ESP_LOGW(TAG, "%s: task name \"%s\" cut to \"%s\"", owner, name, fitted);
    }
    portENTER_CRITICAL(&book_mux);
    int slot = task_book_add(&book, owner, fitted, stack_bytes, (task_class_t)prio, fn, arg);
    portEXIT_CRITICAL(&book_mux);
    if (slot < 0) {
        ESP_LOGE(TAG, "%s: task %s not started: all %d task slots are taken", owner, fitted, TASK_BOOK_SLOTS);
        return false;
    }
    // Straight into the caller's variable: FreeRTOS writes it before the task can run (mesh_task_handle relies on it)
    TaskHandle_t  own = NULL;
    TaskHandle_t *out = handle ? handle : &own;
    if (xTaskCreatePinnedToCore(trampoline, fitted, stack_bytes, (void *)(intptr_t)slot, (UBaseType_t)priority, out,
                                task_class_core((task_class_t)prio)) != pdPASS) {
        portENTER_CRITICAL(&book_mux);
        task_book_remove(&book, slot);
        portEXIT_CRITICAL(&book_mux);
        ESP_LOGE(TAG, "%s: task %s not started: no memory for %u B of stack", owner, fitted, (unsigned)stack_bytes);
        return false;
    }
    return true;
}

size_t lichen_task_leftovers(lichen_app_t const *app, uint32_t wait_ms) {
    char const  *owner = app ? app->id : TASK_BOOK_OS;
    TaskHandle_t self  = xTaskGetCurrentTaskHandle();
    size_t       left  = 0;
    for (uint32_t waited = 0;; waited += LEFTOVER_POLL_MS) {
        portENTER_CRITICAL(&book_mux);
        left = task_book_count(&book, owner, self);
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
        if (s->used && strcmp(s->owner, owner) == 0 && s->handle != self) {
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
        console_reply("task %s owner %s prio %u core %s stack %s free %u", tasks[i].pcTaskName,
                      s ? s->owner : "-", (unsigned)tasks[i].uxCurrentPriority, core, stack,
                      (unsigned)tasks[i].usStackHighWaterMark);
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
    portENTER_CRITICAL(&book_mux);
    memcpy(snap, &book, sizeof(*snap));
    portEXIT_CRITICAL(&book_mux);
    print_tasks(snap, tasks, n);
    size_t booked = 0;
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        booked += snap->slot[i].used;
    }
    console_reply("tasks: %u running, %u of %d task slots taken", (unsigned)n, (unsigned)booked, TASK_BOOK_SLOTS);
    heap_caps_free(tasks);
    heap_caps_free(snap);
}

void lichen_task_console_register(void) {
    console_register("tasks", "tasks (every task with its owner app, priority, core, stack size and bytes never used)",
                     cmd_tasks);
}
