#pragma once

// The gate's check of a caller's context before the memory and task books (#230 E1b): resolve ctx, then book under
// it or refuse. Pure (no ESP-IDF): the context table, the lock and the current-context switch come in through
// gate_ctx_t and gate_lock_t, so host_tests/gate_book_test.c runs the same decisions lichen_mem.c and lichen_task.c do.

#include "mem_book.h"
#include "task_book.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // True and the app's *id and *name (both NULL for the OS) when ctx is valid. False when it is stale, made up or
    // 0: the implementation logs (rate-limited) and counts it under what. Never blocks, takes no lock.
    bool (*resolve)(uint32_t ctx, char const **id, char const **name, char const *what);
    uint32_t (*os)(void);             // the OS's context
    uint32_t (*enter)(uint32_t ctx);  // makes ctx the calling task's current context (lichen_ctx_enter)
} gate_ctx_t;

typedef struct {
    void (*take)(void); // the book's spinlock
    void (*give)(void);
} gate_lock_t;

typedef struct {
    uint32_t    ctx;
    char const *label; // the app's id, its name when it has none; NULL for the OS (MEM_BOOK_OS, TASK_BOOK_OS)
    bool        os;    // ctx is the OS's (not a NULL label): counted, never refused for the quota
} gate_owner_t;

// The owner behind ctx; false when refused (resolve has logged and counted it)
bool gate_owner(gate_ctx_t const *g, uint32_t ctx, char const *what, gate_owner_t *who);

#define MEM_GATE_REFUSED ((mem_charge_t)-1) // ctx refused: nothing booked, the caller allocates nothing

// lichen_alloc's booking: resolves ctx, then charges bytes under it with the lock held. *who is the owner (for the
// release of a failed allocation and the logs); MEM_GATE_REFUSED leaves the book untouched.
mem_charge_t gate_mem_charge(gate_ctx_t const *g, gate_lock_t const *l, mem_book_t *b, uint32_t ctx, bool internal,
                             uint32_t bytes, gate_owner_t *who);

#define MEM_GATE_RELEASE_REFUSED ((mem_release_t)-1) // ctx refused: nothing released, the caller frees nothing

// lichen_free's booking: resolves ctx, then releases bytes under it with the lock held. *label gets the owner's label.
mem_release_t gate_mem_release(gate_ctx_t const *g, gate_lock_t const *l, mem_book_t *b, uint32_t ctx, bool internal,
                               uint32_t bytes, char const **label);

typedef struct {
    char const  *name; // already fitted (task_name_fit)
    uint32_t     stack;
    task_class_t cls;
    task_stack_t mem;
    void (*fn)(void *arg);
    void *arg;
} task_spec_t;

#define TASK_GATE_REFUSED (-2) // ctx refused: the book is untouched, nothing may start

// lichen_task_start's booking: resolves ctx, then books spec under it with the lock held. The slot, -1 when the book
// is full or spec is invalid, TASK_GATE_REFUSED. *who is the owner the task runs and is counted for.
int gate_task_add(gate_ctx_t const *g, gate_lock_t const *l, task_book_t *b, uint32_t ctx, task_spec_t const *spec,
                  gate_owner_t *who);

// The trampoline's start: enters the ctx the task was booked under, for good, then runs fn(arg) on it
void gate_task_run(gate_ctx_t const *g, uint32_t ctx, void (*fn)(void *arg), void *arg);

// lichen_mem.c's, shared with lichen_task.c: the context table of lichen_sys_app.h
extern gate_ctx_t const gate_os_ctx;
