#pragma once

// Who holds the LVGL lock through the UI service (#225), for the debug lock-owner check. Pure (host test:
// ui_call_queue_test.c); tasks are opaque non-zero handles. taken/given run with the lock held, by its holder; check
// may run on any task at any time, so the fields it reads are atomics (relaxed: a task only ever compares the owner
// with itself, and a task's own writes are always visible to it).

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    _Atomic uintptr_t owner;     // the task inside lichen_sys_ui_lock .. unlock, 0 = none
    unsigned          depth;     // the owner's nesting (the port mutex is recursive); only the owner touches it
    _Atomic uintptr_t lvgl_task; // 0 = the check is not armed yet (boot: board's port setup takes the lock directly)
} ui_lock_owner_t;

typedef enum {
    UI_LOCK_OK,        // the LVGL task, or the task holding the lock
    UI_LOCK_UNARMED,   // too early to tell
    UI_LOCK_VIOLATION, // another task touches LVGL without the lock
} ui_lock_verdict_t;

void ui_lock_owner_arm(ui_lock_owner_t *o, uintptr_t lvgl_task);
// Armed yet: the check's cheap first test, before the caller looks up its own task (#225 cost).
bool ui_lock_owner_armed(ui_lock_owner_t *o);
void ui_lock_owner_taken(ui_lock_owner_t *o, uintptr_t self);
// False when self is not the owner (an unlock without its lock): nothing changes.
bool ui_lock_owner_given(ui_lock_owner_t *o, uintptr_t self);
ui_lock_verdict_t ui_lock_owner_check(ui_lock_owner_t *o, uintptr_t self);
