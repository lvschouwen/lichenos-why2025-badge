#pragma once
// The apps' rows for the memory book host tests (#520): on the badge a row lives in the app's own block, here in a small
// array. A ctx finds its row, else the first one nobody uses (ctx 0): FAKE_ROWS owners at most, like a table that is
// full, so the "no row" paths stay testable.
#include "mem_book.h"

#include <string.h>

#define FAKE_ROWS 24

static mem_use_t fake_rows[FAKE_ROWS];

static mem_use_t *fake_app_row(void *user, uint32_t ctx) {
    (void)user;
    mem_use_t *free_row = NULL;
    for (int i = 0; i < FAKE_ROWS; i++) {
        if (fake_rows[i].ctx == ctx) {
            return &fake_rows[i];
        }
        if (!fake_rows[i].ctx && !free_row) {
            free_row = &fake_rows[i];
        }
    }
    return free_row;
}

// An empty book with the fake rows, all free
static void fake_book_reset(mem_book_t *b) {
    memset(fake_rows, 0, sizeof(fake_rows));
    memset(b, 0, sizeof(*b));
    b->app_row = fake_app_row;
}
