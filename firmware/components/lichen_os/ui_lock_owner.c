#include "ui_lock_owner.h"

void ui_lock_owner_arm(ui_lock_owner_t *o, uintptr_t lvgl_task) {
    atomic_store_explicit(&o->lvgl_task, lvgl_task, memory_order_relaxed);
}

bool ui_lock_owner_armed(ui_lock_owner_t *o) {
    return atomic_load_explicit(&o->lvgl_task, memory_order_relaxed) != 0;
}

void ui_lock_owner_taken(ui_lock_owner_t *o, uintptr_t self) {
    if (atomic_load_explicit(&o->owner, memory_order_relaxed) == self) {
        o->depth++;
        return;
    }
    // The mutex is ours now, so nobody else is inside a lichen_sys_ui_lock region (a holder that took the port mutex
    // directly, the LVGL task's handler, never registered here)
    o->depth = 1;
    atomic_store_explicit(&o->owner, self, memory_order_relaxed);
}

bool ui_lock_owner_given(ui_lock_owner_t *o, uintptr_t self) {
    if (atomic_load_explicit(&o->owner, memory_order_relaxed) != self || o->depth == 0) {
        return false;
    }
    if (--o->depth == 0) {
        atomic_store_explicit(&o->owner, 0, memory_order_relaxed);
    }
    return true;
}

ui_lock_verdict_t ui_lock_owner_check(ui_lock_owner_t *o, uintptr_t self) {
    uintptr_t lvgl = atomic_load_explicit(&o->lvgl_task, memory_order_relaxed);
    if (lvgl == 0) {
        return UI_LOCK_UNARMED;
    }
    if (self == lvgl || atomic_load_explicit(&o->owner, memory_order_relaxed) == self) {
        return UI_LOCK_OK;
    }
    return UI_LOCK_VIOLATION;
}
