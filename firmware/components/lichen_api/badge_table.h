#pragma once
// The table behind lichen_badge.h (#102). Pure, host-tested in host_tests/badge_table_test.c; lichen_badge.c adds the
// lock and the registered-app check.

#include <stdbool.h>
#include <stdint.h>

#define BADGE_ID_CAP 14 // an app id is 1..13 of a-z 0-9 _ (app_ns.h)
#define BADGE_SLOTS  16 // LICHEN_MAX_APPS (lichen_badge.c checks); lichen_app.h needs LVGL, this is host-built

typedef struct {
    struct {
        char     id[BADGE_ID_CAP];
        unsigned count;
    } slot[BADGE_SLOTS];
    uint32_t generation;
} badge_table_t;

// True when the count changed (the generation moved). An id that is not a valid app id, or a new id while every slot
// holds a count, is ignored. A slot whose count goes to 0 is freed.
bool     badge_table_set(badge_table_t *t, char const *id, unsigned count);
unsigned badge_table_get(badge_table_t const *t, char const *id);
