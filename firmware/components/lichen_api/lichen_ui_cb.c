// Event and timer callbacks that run as their app (#349, ui_cb.c has the record). The registration keeps the caller's
// context (lichen_ctx_self) in a record LVGL holds as the descriptor's or timer's external data, and a trampoline enters
// it around the real callback: the memory and task books, the capability check (#229) and the panic hook's blame (#232:
// it reads the same thread-local slot, and the LVGL task's TCB is in internal RAM) then see the app, not the OS. LVGL
// frees the record with the descriptor or the timer (CONFIG_LV_USE_EXT_DATA). No lock: the callers already run on the
// LVGL task under it, and so do the trampolines.
#include "lichen_sys_app.h"
#include "lichen_sys_life.h"
#include "lichen_timer.h"
#include "lichen_ui.h"
#include "timer_book.h"
#include "ui_cb.h"

#include "app_ctx.h"
#include "esp_log.h"

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

// ---- booked timers (#231, lichen_timer.h) ---------------------------------------------------------------------------
// A booked timer is a #349 wrapper timer (so it runs as its app) whose user data is its handle in the book. The book is
// a list of claims, LVGL's timer list is the truth: a timer is only deleted after it was found there with this file's
// trampoline, record and handle, so a timer the app deleted itself (lv_timer_delete), or memory LVGL reused for another
// timer, is never deleted twice. Internal .bss (~0.3 KB), the LVGL task only (all callers hold the LVGL lock).

static char const TIMER_TAG[] = "timer";

static timer_book_t s_timers;
static uint8_t      s_timer_count[APP_CTX_SLOTS]; // per context slot: stop(reason) reads it from its own task

static void count_note(uint32_t ctx) {
    uint32_t slot = ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
    if (slot < APP_CTX_SLOTS) {
        __atomic_store_n(&s_timer_count[slot], (uint8_t)timer_book_count(&s_timers, ctx), __ATOMIC_RELAXED);
    }
}

static void booked_cb(lv_timer_t *t) {
    timer_slot_t const *slot = timer_book_find(&s_timers, (uint32_t)(uintptr_t)t->user_data);
    if (slot) {
        slot->cb(slot->arg); // may stop its own timer, which clears the slot: nothing reads it afterwards
    }
}

static lv_timer_t *live_timer(uint32_t handle) {
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
        ui_cb_rec_t const *rec = t->ext_data.data;
        if (t->timer_cb == timer_trampoline && t->ext_data.free_cb == rec_free && rec && rec->fn.timer == booked_cb &&
            (uint32_t)(uintptr_t)t->user_data == handle) {
            return t;
        }
    }
    return NULL;
}

// The book is full: claims whose timer is gone (deleted by the app itself) are let go
static void timers_tidy(void) {
    for (int i = 0; i < TIMER_BOOK_SLOTS; i++) {
        uint32_t h = s_timers.slot[i].handle;
        if (h && !live_timer(h)) {
            uint32_t ctx = s_timers.slot[i].ctx;
            timer_book_remove(&s_timers, h);
            count_note(ctx);
        }
    }
}

lichen_timer_t lichen_timer_start(lichen_ctx_t ctx, uint32_t period_ms, void (*cb)(void *arg), void *arg) {
    if (!cb || !period_ms || !lichen_sys_ctx_resolve(ctx, NULL, "lichen_timer_start")) {
        return LICHEN_TIMER_NONE; // a refused ctx is logged and counted by the context table
    }
    uint32_t h = timer_book_add(&s_timers, ctx, cb, arg);
    if (!h) {
        timers_tidy();
        h = timer_book_add(&s_timers, ctx, cb, arg);
    }
    if (!h) {
        ESP_LOGW(TIMER_TAG, "no booked timer left (%d in use)", TIMER_BOOK_SLOTS);
        return LICHEN_TIMER_NONE;
    }
    lichen_ctx_t previous = lichen_ctx_enter(ctx); // the wrapper's record keeps the context it was made in
    lv_timer_t  *t        = lichen_ui_timer_create(booked_cb, period_ms, (void *)(uintptr_t)h);
    lichen_ctx_leave(previous);
    if (!t) {
        timer_book_remove(&s_timers, h);
        return LICHEN_TIMER_NONE;
    }
    count_note(ctx);
    return h;
}

bool lichen_timer_stop(lichen_ctx_t ctx, lichen_timer_t timer) {
    timer_slot_t const *slot = timer_book_find(&s_timers, timer);
    if (!slot || slot->ctx != ctx) {
        return false;
    }
    lv_timer_t *t = live_timer(timer);
    timer_book_remove(&s_timers, timer);
    count_note(ctx);
    if (t) {
        lv_timer_delete(t);
    }
    return true;
}

size_t lichen_sys_timer_count(uint32_t ctx) {
    uint32_t slot = ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
    return slot < APP_CTX_SLOTS ? __atomic_load_n(&s_timer_count[slot], __ATOMIC_RELAXED) : 0;
}

void lichen_sys_timer_drop(uint32_t ctx) {
    size_t   from = 0, dropped = 0;
    uint32_t h;
    while ((h = timer_book_next(&s_timers, ctx, &from)) != 0) {
        lv_timer_t *t = live_timer(h);
        timer_book_remove(&s_timers, h);
        if (t) {
            lv_timer_delete(t);
            dropped++;
        }
    }
    count_note(ctx);
    if (dropped) {
        ESP_LOGI(TIMER_TAG, "ctx 0x%08lx: %u booked timer(s) dropped with its stop", (unsigned long)ctx,
                 (unsigned)dropped);
    }
}
