// Tile badges (#102): lichen_badge.h over badge_table.c, under a spinlock the mesh and LVGL tasks share
#include "lichen_badge.h"

#include "badge_table.h"
#include "freertos/FreeRTOS.h"
#include "lichen_app.h"

#include <assert.h>

static_assert(BADGE_SLOTS >= LICHEN_MAX_APPS, "a badge slot for every app");

static badge_table_t s_table;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

void lichen_badge_set(char const *app_id, unsigned count) {
    if (!lichen_app_by_id(app_id)) {
        return; // the app table is filled before any app runs, and only read afterwards
    }
    portENTER_CRITICAL(&s_mux);
    badge_table_set(&s_table, app_id, count);
    portEXIT_CRITICAL(&s_mux);
}

unsigned lichen_badge_get(char const *app_id) {
    portENTER_CRITICAL(&s_mux);
    unsigned count = badge_table_get(&s_table, app_id);
    portEXIT_CRITICAL(&s_mux);
    return count;
}

uint32_t lichen_badge_generation(void) {
    portENTER_CRITICAL(&s_mux);
    uint32_t generation = s_table.generation;
    portEXIT_CRITICAL(&s_mux);
    return generation;
}
