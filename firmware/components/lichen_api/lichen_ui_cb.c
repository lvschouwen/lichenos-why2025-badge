// Event and timer callbacks that run as their app (#349, ui_cb.c has the record). The registration keeps the caller's
// context (lichen_ctx_self) in a record LVGL holds as the descriptor's or timer's external data, and a trampoline enters
// it around the real callback: the memory and task books, the capability check (#229) and the panic hook's blame (#232:
// it reads the same thread-local slot, and the LVGL task's TCB is in internal RAM) then see the app, not the OS. LVGL
// frees the record with the descriptor or the timer (CONFIG_LV_USE_EXT_DATA). No lock: the callers already run on the
// LVGL task under it, and so do the trampolines.
#include "lichen_sys_app.h"
#include "lichen_ui.h"
#include "ui_cb.h"

#include "lvgl.h"
#include "lvgl_private.h"

#if !LV_USE_EXT_DATA
#error "lichen_ui_cb.c needs CONFIG_LV_USE_EXT_DATA (firmware/sdkconfig.defaults)"
#endif

static void *rec_alloc(size_t size) {
    return lv_malloc(size); // LVGL's heap: PSRAM (board/lv_mem_psram.c), like the descriptor itself
}

// The mark spans the whole time the app's context is entered: a crash in there is named after the app but is no strike
// against it (#349 review), since the shell, LVGL's renderer and the PSRAM path a callback reaches run as the app too
static uint32_t cb_enter(uint32_t ctx) {
    lichen_sys_ui_cb_mark(true);
    return lichen_ctx_enter(ctx);
}

static void cb_leave(uint32_t previous) {
    lichen_ctx_leave(previous);
    lichen_sys_ui_cb_mark(false);
}

static ui_cb_env_t const s_env = {
    .alloc = rec_alloc,
    .free  = lv_free,
    .self  = lichen_ctx_self,
    .enter = cb_enter,
    .leave = cb_leave,
};

static void rec_free(void *rec) {
    ui_cb_free(&s_env, rec);
}

// LVGL copies the descriptor's external data into the event before each call (lv_event_send)
static void event_trampoline(lv_event_t *e) {
    ui_cb_run_event(&s_env, e->ext_data.data, e);
}

static void timer_trampoline(lv_timer_t *t) {
    ui_cb_run_timer(&s_env, t->ext_data.data, t);
}

static void group_trampoline(lv_group_t *g) {
    ui_cb_run_group(&s_env, g->ext_data.data, g);
}

// The group's external data is this record's alone (no other code sets one): the old record goes before a new one
void lichen_ui_group_focus_cb(lv_group_t *group, lv_group_focus_cb_t cb) {
    if (!group) {
        return;
    }
    ui_cb_rec_t *rec = ui_cb_new_group(&s_env, cb); // NULL for cb NULL (or no memory): no focus callback then
    lv_group_set_focus_cb(group, rec ? group_trampoline : NULL);
    if (group->ext_data.free_cb == rec_free) {
        ui_cb_free(&s_env, group->ext_data.data);
    }
    lv_group_set_external_data(group, rec, rec ? rec_free : NULL);
}

lv_event_dsc_t *lichen_ui_event_add(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t filter, void *user_data) {
    if (!obj) {
        return NULL;
    }
    ui_cb_rec_t *rec = ui_cb_new_event(&s_env, cb);
    if (!rec) {
        return NULL;
    }
    lv_event_dsc_t *dsc = lv_obj_add_event_cb(obj, event_trampoline, filter, user_data);
    if (!dsc) {
        ui_cb_free(&s_env, rec);
        return NULL;
    }
    lv_event_desc_set_external_data(dsc, rec, rec_free);
    return dsc;
}

lv_timer_t *lichen_ui_timer_create(lv_timer_cb_t cb, uint32_t period_ms, void *user_data) {
    ui_cb_rec_t *rec = ui_cb_new_timer(&s_env, cb);
    if (!rec) {
        return NULL;
    }
    lv_timer_t *t = lv_timer_create(timer_trampoline, period_ms, user_data);
    if (!t) {
        ui_cb_free(&s_env, rec);
        return NULL;
    }
    lv_timer_set_external_data(t, rec, rec_free);
    return t;
}
