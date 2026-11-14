// Tile badges (#102): lichen_badge.h over badge_table.c, under a spinlock the mesh and LVGL tasks share
#include "lichen_badge.h"

#include "badge_table.h"
#include "freertos/FreeRTOS.h"
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_sys_badge.h"

#include <assert.h>

static_assert(BADGE_SLOTS >= LICHEN_MAX_APPS, "a badge slot for every app");

static badge_table_t s_table;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

void lichen_sys_badge_set(char const *app_id, unsigned count) {
    if (!lichen_app_by_id(app_id)) {
        return; // the app table is filled before any app runs, and only read afterwards
    }
    portENTER_CRITICAL(&s_mux);
    badge_table_set(&s_table, app_id, count);
    portEXIT_CRITICAL(&s_mux);
}

unsigned lichen_sys_badge_get(char const *app_id) {
    portENTER_CRITICAL(&s_mux);
    unsigned count = badge_table_get(&s_table, app_id);
    portEXIT_CRITICAL(&s_mux);
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
    portENTER_CRITICAL(&s_mux);
    uint32_t generation = s_table.generation;
    portEXIT_CRITICAL(&s_mux);
    return generation;
}
