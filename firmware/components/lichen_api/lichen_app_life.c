// The apps' lifecycle records (#231) behind lichen_sys_life.h: app_life.c's transitions over the record in each app's
// block (app_state.h, #520: PSRAM, made and freed with the app, so the apps cost no internal RAM and there is no limit
// on them), under the app lock (nothing in here logs inside it). A mutex and not a spinlock because the records are PSRAM, which no critical section may touch (#271). Read and
// written from the OTA, power, console and LVGL tasks.
#include "lichen_sys_life.h"
#include "lichen_sys_app.h"

#include "app_ctx.h"
#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "screen_ids.h"

#include <string.h>

static char const TAG[] = "life";

// The app lock (lichen_sys_app_lock), recursive: the record lookup below takes it again for the app list
static bool life_take(void) {
    return lichen_sys_app_lock();
}

static void life_give(bool taken) {
    lichen_sys_app_unlock(taken);
}

static uint32_t now_ms(void) {
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

// The record of the app at index (NULL past the table), found under the lock: the block is freed when the app leaves,
// and an app only leaves while its record says it is not running (lichen_sys_life_removable)
static app_life_t *record_at(size_t index) {
    app_state_t *st = lichen_sys_app_state(lichen_sys_app_ctx_at(index));
    return st ? &st->life : NULL;
}

static char const *app_label(size_t index) {
    lichen_app_t const *app = lichen_app_at(index);
    return app ? (app->id ? app->id : app->name) : "?";
}

void lichen_sys_life_started(size_t index) {
    lichen_ctx_t ctx = lichen_sys_app_ctx_at(index);
    bool taken = life_take();
    app_life_t *r  = record_at(index);
    bool        ok = r && app_life_started(r);
    life_give(taken);
    if (ok) {
        lichen_sys_timer_revive(ctx); // a restarted app may book timers again
    } else {
        ESP_LOGW(TAG, "%s: background start not recorded", app_label(index));
    }
}

void lichen_sys_life_fault_crashes(size_t index) {
    bool taken = life_take();
    app_life_t *r = record_at(index);
    if (r) {
        app_life_fault_crashes(r);
    }
    life_give(taken);
}

// The slot of the app ctx is the context of, 0 for the OS's or one that is not an app's (a stale ctx has no block)
static int owner_slot(uint32_t ctx) {
    return lichen_sys_app_state(ctx) ? (int)(ctx & ((1u << APP_CTX_SLOT_BITS) - 1u)) : 0;
}

void lichen_sys_life_screens(int const *ids, uint32_t const *ctxs, int depth) {
    size_t apps = lichen_app_count();
    int    owners[LIFE_SCREENS_MAX] = {0};
    depth = depth < LIFE_SCREENS_MAX ? depth : LIFE_SCREENS_MAX;
    // The owner of a screen is an app's context slot: its own screen's id is SCREEN_APP_OWN + slot, its page's
    // SCREEN_APP_BASE + slot (screen_ids.h), else the app whose context opened it
    for (int f = 0; f < depth; f++) {
        owners[f] = app_life_screen_owner(ids[f], owner_slot(ctxs[f]), SCREEN_APP_OWN + 1,
                                          SCREEN_APP_OWN + SCREEN_APP_MAX, SCREEN_APP_BASE + 1);
    }
    for (size_t i = 0; i < apps; i++) {
        lichen_ctx_t ctx  = lichen_sys_app_ctx_at(i);
        int          slot = (int)(ctx & ((1u << APP_CTX_SLOT_BITS) - 1u));
        bool         shown, on_top;
        app_life_view(owners, depth, slot, &shown, &on_top);
        bool taken = life_take();
        app_life_t *r       = record_at(i);
        bool        changed = r && app_life_show(r, shown, on_top);
        uint8_t     state   = r ? r->state : 0;
        life_give(taken);
        if (changed) {
            ESP_LOGD(TAG, "%s %s", app_label(i), app_life_state_name((life_state_t)state));
        }
    }
}

life_begin_t lichen_sys_life_stop_begin(size_t index, life_reason_t reason) {
    uint32_t now = now_ms();
    bool taken = life_take();
    app_life_t  *r   = record_at(index);
    life_begin_t got = r ? app_life_stop_begin(r, reason, now) : LIFE_BEGIN_BAD;
    life_give(taken);
    return got;
}

life_state_t lichen_sys_life_stop_end(size_t index, bool stopped, uint32_t tasks_left) {
    uint32_t now = now_ms();
    bool taken = life_take();
    app_life_t  *r   = record_at(index);
    life_state_t got = r ? app_life_stop_end(r, stopped, now) : LIFE_STATE_COUNT;
    if (r && got != LIFE_STATE_COUNT) {
        app_life_note_tasks(r, tasks_left);
    }
    life_give(taken);
    return got;
}

life_wait_t lichen_sys_life_wait(size_t index, life_reason_t own, uint32_t since_ms) {
    uint32_t now = now_ms();
    bool taken = life_take();
    app_life_t *r   = record_at(index);
    life_wait_t got = r ? app_life_wait_verdict(r, own, since_ms, now) : LIFE_WAIT_FAILED;
    life_give(taken);
    return got;
}

bool lichen_sys_life_removable(lichen_ctx_t ctx) {
    bool taken = life_take();
    app_state_t *st = lichen_sys_app_state(ctx);
    bool         ok = st && app_life_removable((life_state_t)st->life.state);
    life_give(taken);
    return ok;
}

uint32_t lichen_sys_life_now_ms(void) {
    return now_ms();
}

bool lichen_sys_life_get(size_t index, app_life_t *out) {
    bool taken = life_take();
    app_life_t const *r = record_at(index);
    if (r) {
        *out = *r;
    }
    life_give(taken);
    return r != NULL;
}

void lichen_sys_life_console(void) {
    size_t apps = lichen_app_count();
    for (size_t i = 0; i < apps; i++) {
        app_life_t r;
        if (!lichen_sys_life_get(i, &r)) {
            continue; // the app left since the count
        }
        // One line per app; the stop part reads "0 stops" until one happened
        console_reply("app life %s: %s, fault %s, %u timers, %u stops (last %s, %u ms, %u tasks left)", app_label(i),
                      app_life_state_name((life_state_t)r.state), app_life_fault_name((life_fault_t)r.fault),
                      (unsigned)lichen_sys_timer_count(lichen_sys_app_ctx_at(i)), (unsigned)r.stops,
                      r.stops ? app_life_reason_name((life_reason_t)r.reason) : "-", (unsigned)r.took_ms,
                      (unsigned)r.tasks_left);
    }
    console_reply("app life: %u apps", (unsigned)apps);
}
