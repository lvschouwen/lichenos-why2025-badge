#pragma once

// The record behind lichen_ui_event_add and lichen_ui_timer_create (#349, lichen_ui_cb.c): an LVGL event or timer
// callback runs as the context that registered it, not as whatever the LVGL task happens to run as. The record holds
// that context and the real callback; LVGL keeps it as the descriptor's or timer's external data and frees it with them
// (CONFIG_LV_USE_EXT_DATA), so a closed screen or a deleted timer leaves nothing behind. Pure (no ESP-IDF, no LVGL: the
// two LVGL types are only named), host-tested in host_tests/ui_cb_test.c.
//
// Running a record takes no lock and allocates nothing: enter, call, leave. The callback may delete the object or the
// timer, which frees the record: nothing reads it after the call.

#include "app_ctx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct _lv_event_t; // lv_event_t
struct _lv_timer_t; // lv_timer_t
struct _lv_group_t; // lv_group_t

typedef struct {
    uint32_t ctx; // the lichen_ctx_t the registering code ran as
    union {
        void (*event)(struct _lv_event_t *e);
        void (*timer)(struct _lv_timer_t *t);
        void (*group)(struct _lv_group_t *g); // a group's focus callback
    } fn;
} ui_cb_rec_t;

// What enter hands to leave: the task's context before the enter, and the context the outer callback had marked (the
// two differ when that callback entered another app's context in between, #427)
typedef struct {
    uint32_t previous;
    uint32_t outer_mark;
} ui_cb_saved_t;

// What the record works with: lv_malloc / lv_free, lichen_ctx_self, and the mark plus lichen_ctx_enter / _leave on
// the badge; stand-ins in the host test
typedef struct {
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    uint32_t (*self)(void);
    ui_cb_saved_t (*enter)(uint32_t ctx);
    void (*leave)(ui_cb_saved_t saved);
} ui_cb_env_t;

// A record for an event callback (fn not NULL) that will run as the caller's current context. NULL when out of memory.
ui_cb_rec_t *ui_cb_new_event(ui_cb_env_t const *env, void (*fn)(struct _lv_event_t *e));
// The same for a timer callback
ui_cb_rec_t *ui_cb_new_timer(ui_cb_env_t const *env, void (*fn)(struct _lv_timer_t *t));
// The same for a group's focus callback
ui_cb_rec_t *ui_cb_new_group(ui_cb_env_t const *env, void (*fn)(struct _lv_group_t *g));
// Frees a record (NULL is fine): LVGL's free_cb when the descriptor or timer goes
void ui_cb_free(ui_cb_env_t const *env, void *rec);
// Runs the record's callback inside its context and puts the caller's back, also when the callback deleted the
// record's object. A NULL record (one LVGL already let go) does nothing.
void ui_cb_run_event(ui_cb_env_t const *env, ui_cb_rec_t const *rec, struct _lv_event_t *e);
void ui_cb_run_timer(ui_cb_env_t const *env, ui_cb_rec_t const *rec, struct _lv_timer_t *t);
void ui_cb_run_group(ui_cb_env_t const *env, ui_cb_rec_t const *rec, struct _lv_group_t *g);

// ---- the mark the panic hook reads (#349, #427) ---------------------------------------------------------------------
// The task inside an app's callback and the context that callback entered, so a crash there is named after the app but
// is no strike against it (safe_mode_policy.h) only while that context is still the one the task runs as: a callback
// that enters another app's context (Settings running an app's settings or about hook) leaves the mark, and a crash in
// that hook strikes that app. Written by the LVGL task only (it holds the LVGL lock; the trampolines run nowhere else),
// read by the panic handler on any core: internal RAM (plain .bss), no lock.
//
// The stores are ordered so that every state a reader can catch is safe. Each word is one aligned 32-bit store, so it
// is never torn. Entering at depth 0 writes ctx before task: a crash between the two sees no task, so no mark (it
// counts as whatever context the task runs as, the OS's before the trampoline entered the app's). Leaving to depth 0
// clears task first, so a stale ctx is never paired with a live task. Nested, only ctx changes, and a leave puts back
// the ctx the outer callback marked (not the task's context before the inner enter: that may be another app's, entered
// by the outer callback). The one window left: nested, between the mark and the context switch (in either direction)
// only OS code from flash runs; a fault there finds two different slots and strikes the context the task runs as,
// which is the outer callback's app (or the inner one's), never the OS; MeshCore's exemption still applies to it.
typedef struct {
    void *volatile    task;  // the task inside a callback; NULL: none
    volatile uint32_t ctx;   // the context the innermost callback entered
    uint32_t          depth; // the writer's own count, never read by the panic handler
} ui_cb_mark_t;

// The trampoline is about to enter ctx (before env->enter) on task. Returns the ctx marked before, for the leave.
uint32_t ui_cb_mark_enter(ui_cb_mark_t *m, void *task, uint32_t ctx);
// The trampoline left (after env->leave): outer_mark is what ui_cb_mark_enter returned, put back when they nest
void ui_cb_mark_leave(ui_cb_mark_t *m, uint32_t outer_mark);

// The panic handler's test: task (the core's current one) is inside a callback whose context slot is still the one it
// runs as (current_ctx, its thread-local slot). Always inlined: the caller is in IRAM and must not reach flash.
static inline __attribute__((always_inline)) bool ui_cb_mark_holds(ui_cb_mark_t const *m, void const *task,
                                                                    uint32_t current_ctx) {
    void *marked = m->task; // read the task first: ctx was written before it
    uint32_t ctx = m->ctx;
    uint32_t mask = (1u << APP_CTX_SLOT_BITS) - 1u;
    return marked && marked == task && (ctx & mask) != 0 && (ctx & mask) == (current_ctx & mask);
}
