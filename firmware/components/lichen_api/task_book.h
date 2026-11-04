#pragma once
// The bookkeeping behind lichen_task_start (#181): which app owns which task, under which name, stack and priority
// class. Pure, host-tested in host_tests/task_book_test.c; lichen_task.c is the FreeRTOS glue (and the lock: these
// calls are not thread-safe by themselves).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_BOOK_SLOTS    32 // tasks started through lichen_task_start that run at the same time
#define TASK_BOOK_NAME_CAP 16 // FreeRTOS's configMAX_TASK_NAME_LEN with the NUL; lichen_task.c asserts they match
#define TASK_BOOK_OWNER_CAP 14 // an app id is at most 13 characters (app_ns.h)
#define TASK_BOOK_OS       "lichen" // the owner of the OS's own tasks (app NULL)

// The priority classes of lichen_task.h, mapped to FreeRTOS priorities and cores here so the host test pins them
typedef enum {
    TASK_CLASS_BACKGROUND, // 2, core 0: samplers, slow saves
    TASK_CLASS_SERVICE,    // 3, core 0: watchers and longer scans
    TASK_CLASS_JOB,        // 4, core 0: one-shot jobs a screen or command waits for
    TASK_CLASS_URGENT,     // 5, core 0: power off and the like
    TASK_CLASS_APP_LOOP,   // 3, core 1: an app's main loop, next to LVGL and away from SDIO/tcpip/LoRa on core 0
    TASK_CLASS_COUNT,
} task_class_t;

int task_class_priority(task_class_t cls); // -1 for an unknown class
int task_class_core(task_class_t cls);     // -1 for an unknown class

// name cut to what FreeRTOS keeps (TASK_BOOK_NAME_CAP - 1 characters); true when it had to be cut. NULL is "".
bool task_name_fit(char out[TASK_BOOK_NAME_CAP], char const *name);

typedef struct {
    bool     used;
    char     owner[TASK_BOOK_OWNER_CAP];
    char     name[TASK_BOOK_NAME_CAP];
    uint32_t stack;
    uint8_t  cls;
    void    *handle; // the task's handle once known (NULL in between)
    void (*fn)(void *arg);
    void *arg;
} task_slot_t;

typedef struct {
    task_slot_t slot[TASK_BOOK_SLOTS];
} task_book_t;

// A slot for a new task; -1 when the book is full or the class is unknown. owner NULL is the OS. The name is cut as
// task_name_fit does.
int  task_book_add(task_book_t *b, char const *owner, char const *name, uint32_t stack, task_class_t cls,
                   void (*fn)(void *), void *arg);
void task_book_remove(task_book_t *b, int slot); // out of range or already free: nothing happens
// The owner's tasks still in the book, except the one with handle skip (NULL skips none)
size_t task_book_count(task_book_t const *b, char const *owner, void const *skip);

#ifdef __cplusplus
}
#endif
