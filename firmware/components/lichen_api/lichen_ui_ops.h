#pragma once
// The shell's half of lichen_ui.h and lichen_keys.h (#221): navigation, the dialog, the pocket lock, the station clock
// and the raw keys are the shell's work, but apps and the gate's own widgets call them through the gate. The gate
// defines those functions (lichen_ui_ops.c) and forwards each call to the table the shell hands it at boot, so nothing
// below the shell links to a shell definition (an upward edge, debt V2 of #216).
//
// Before the table is set, and for a member left NULL, every forwarded call has a defined answer: a void call does
// nothing, a bool one is false, a pointer NULL, a count 0, and lichen_date_text leaves "" in out. The shell sets the table
// in shell_boot_screen (the boot screen already styles itself through shell_style_screen) and again in shell_start;
// without a display neither runs and the calls keep those answers. A forwarded call keeps the thread rule its
// lichen_ui.h / lichen_keys.h comment states: the forwarding is one atomic load, it takes no lock of its own.

#include "lichen_keys.h"
#include "lichen_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // The gate's own widgets (menu.c) defer work to the LVGL task with these: the UI service is lichen_os, which the
    // gate may not call (#286). The shell binds lichen_sys_ui_call and lichen_sys_ui_cancel.
    bool (*later)(void (*fn)(void *), void *arg);
    void (*later_cancel)(void (*fn)(void *), void *arg);
    // lichen_ui.h's navigation; shell_open is open_keyed with key 0, shell_key_hash is the gate's own (pure)
    void (*home)(void);
    void (*back)(void);
    void (*open_keyed)(lv_obj_t *(*build)(void), int screen_id, uint32_t key);
    unsigned (*open_reuse_count)(void);
    void (*open_count_reuse)(void);
    bool (*screen_open)(int screen_id, uint32_t key);
    void (*on_esc)(bool (*fn)(void *arg), void *arg);
    void (*on_search)(void (*fn)(void *arg), void *arg);
    void (*scroll_by)(lv_obj_t *obj, int32_t dy);
    lv_obj_t *(*page_create)(void);
    void (*scroll_tame)(lv_obj_t *obj);
    void (*no_arrow_scroll)(lv_obj_t *obj);
    bool (*back_then)(void (*then)(void *), void *arg);
    bool (*back_to)(lv_obj_t *screen, void (*then)(void *), void *arg);
    lv_obj_t *(*stack_screen)(int from_top);
    void (*style_screen)(lv_obj_t *screen);
    // the countdown dialog and the pocket lock
    bool (*dialog_countdown)(shell_dialog_t const *d, shell_dialog_fn done);
    void (*dialog_close)(void);
    bool (*screen_locked)(void);
    // the station clock
    bool (*station_clock)(lv_obj_t *screen, void (*update_line2)(lv_obj_t *line2));
    bool (*date_text)(char *out, size_t cap);
    // lichen_keys.h
    bool (*keys_raw_take)(lichen_raw_key_fn fn, void *arg);
    void (*keys_raw_release)(lichen_raw_key_fn fn, void *arg);
} lichen_ui_ops_t;

// The shell's table, or NULL to take it away (host tests). ops must outlive every call (a static const table); setting
// the same table again changes nothing. The shell only: apps may not call it (a forbid line in tools/layers.conf).
// Any task (an atomic store; a call already forwarded finishes on the old table).
void lichen_ui_ops_set(lichen_ui_ops_t const *ops);

// For the gate's own code only (menu.c; apps use lichen_ui_call, a forbid line in tools/layers.conf): runs fn(arg) on
// the LVGL task soon, FIFO with every other UI-service call, never inline. False when it was not queued (no table yet,
// or the UI service refused): fn will not run. LVGL task (the shell's call takes the lock, which that task holds).
bool lichen_ui_ops_later(void (*fn)(void *), void *arg);
// Takes back every fn(arg) lichen_ui_ops_later queued that has not run yet (a row being deleted). LVGL task.
void lichen_ui_ops_later_cancel(void (*fn)(void *), void *arg);
