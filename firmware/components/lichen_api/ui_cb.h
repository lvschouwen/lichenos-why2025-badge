#pragma once

// The record behind lichen_ui_event_add and lichen_ui_timer_create (#349, lichen_ui_cb.c): an LVGL event or timer
// callback runs as the context that registered it, not as whatever the LVGL task happens to run as. The record holds
// that context and the real callback; LVGL keeps it as the descriptor's or timer's external data and frees it with them
// (CONFIG_LV_USE_EXT_DATA), so a closed screen or a deleted timer leaves nothing behind. Pure (no ESP-IDF, no LVGL: the
// two LVGL types are only named), host-tested in host_tests/ui_cb_test.c.
//
// Running a record takes no lock and allocates nothing: enter, call, leave. The callback may delete the object or the
// timer, which frees the record: nothing reads it after the call.

#include <stddef.h>
#include <stdint.h>

struct _lv_event_t; // lv_event_t
struct _lv_timer_t; // lv_timer_t

typedef struct {
    uint32_t ctx; // the lichen_ctx_t the registering code ran as
    union {
        void (*event)(struct _lv_event_t *e);
        void (*timer)(struct _lv_timer_t *t);
    } fn;
} ui_cb_rec_t;

// What the record works with: lv_malloc / lv_free and lichen_ctx_self / _enter / _leave on the badge, stand-ins in
// the host test
typedef struct {
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    uint32_t (*self)(void);
    uint32_t (*enter)(uint32_t ctx);
    void (*leave)(uint32_t previous);
} ui_cb_env_t;

// A record for an event callback (fn not NULL) that will run as the caller's current context. NULL when out of memory.
ui_cb_rec_t *ui_cb_new_event(ui_cb_env_t const *env, void (*fn)(struct _lv_event_t *e));
// The same for a timer callback
ui_cb_rec_t *ui_cb_new_timer(ui_cb_env_t const *env, void (*fn)(struct _lv_timer_t *t));
// Frees a record (NULL is fine): LVGL's free_cb when the descriptor or timer goes
void ui_cb_free(ui_cb_env_t const *env, void *rec);
// Runs the record's callback inside its context and puts the caller's back, also when the callback deleted the
// record's object. A NULL record (one LVGL already let go) does nothing.
void ui_cb_run_event(ui_cb_env_t const *env, ui_cb_rec_t const *rec, struct _lv_event_t *e);
void ui_cb_run_timer(ui_cb_env_t const *env, ui_cb_rec_t const *rec, struct _lv_timer_t *t);
