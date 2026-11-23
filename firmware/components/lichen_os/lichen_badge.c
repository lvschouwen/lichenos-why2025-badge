// Tile badges (#102): lichen_badge.h over badge_table.c. The count of an app is in its block (app_state.h, #520), under the
// app lock the mesh and LVGL tasks share: the block is PSRAM, which no critical section may touch (#271).
#include "lichen_badge.h"

#include "app_state.h"
#include "badge_table.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_sys_badge.h"

#include <string.h>

static uint32_t          s_generation; // moves when any count changes (read under the lock)

// The app lock (lichen_sys_app_lock): false when the caller may not block, then not given either
static bool take(void) {
    return lichen_sys_app_lock();
}

static void give(bool taken) {
    lichen_sys_app_unlock(taken);
}

// The context of the app with that id (an alias is no tile), LICHEN_CTX_NONE when there is none
static lichen_ctx_t ctx_of_id(char const *app_id) {
    for (size_t i = 0; app_id && i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (app && app->id && strcmp(app->id, app_id) == 0) {
            return lichen_sys_app_ctx_at(i);
        }
    }
    return LICHEN_CTX_NONE;
}

void lichen_sys_badge_set(char const *app_id, unsigned count) {
    lichen_ctx_t ctx = ctx_of_id(app_id);
    if (ctx == LICHEN_CTX_NONE) {
        return;
    }
    bool taken = take();
    app_state_t *st = lichen_sys_app_state(ctx);
    if (st) {
        badge_count_set(&st->badge, &s_generation, count);
    }
    give(taken);
}

unsigned lichen_sys_badge_get(char const *app_id) {
    lichen_ctx_t ctx = ctx_of_id(app_id);
    if (ctx == LICHEN_CTX_NONE) {
        return 0;
    }
    bool taken = take();
    app_state_t *st    = lichen_sys_app_state(ctx);
    unsigned     count = st ? st->badge : 0;
    give(taken);
    return count;
}

// The tile of the app ctx stands for, NULL for a refused ctx (logged by the context table), the OS or an id-less app
static char const *own_tile(lichen_ctx_t ctx, char const *what) {
    char const *id = NULL, *name = NULL;
    return lichen_sys_ctx_app(ctx, what, &id, &name) == GATE_APP_ID ? id : NULL;
}

void lichen_badge_set(lichen_ctx_t ctx, unsigned count) {
    if (!lichen_sys_caps_ok(ctx, CAP_NOTIFY, "lichen_badge_set")) {
        return; // #229: the tile badge is a notification
    }
    char const *id = own_tile(ctx, "lichen_badge_set");
    if (id) {
        lichen_sys_badge_set(id, count);
    }
}

unsigned lichen_badge_get(lichen_ctx_t ctx) {
    char const *id = own_tile(ctx, "lichen_badge_get");
    return id ? lichen_sys_badge_get(id) : 0;
}

uint32_t lichen_badge_generation(void) {
    bool taken = take();
    uint32_t generation = s_generation;
    give(taken);
    return generation;
}
