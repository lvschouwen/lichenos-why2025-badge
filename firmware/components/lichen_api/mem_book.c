// lichen_alloc's accounting (#181), see mem_book.h. Pure: no heap calls here.
#include "mem_book.h"

#include <string.h>

static void owner_fit(char out[MEM_BOOK_OWNER_CAP], char const *owner) {
    if (!owner) {
        owner = MEM_BOOK_OS;
    }
    size_t n = strlen(owner);
    if (n >= MEM_BOOK_OWNER_CAP) {
        n = MEM_BOOK_OWNER_CAP - 1;
    }
    memcpy(out, owner, n);
    out[n] = 0;
}

// make: a new row labelled label (os: the OS's) when ctx has none
static mem_use_t *row_of(mem_book_t *b, uint32_t ctx, bool make, char const *label, bool os) {
    if (!ctx) {
        return NULL; // 0 marks a free row and is never a valid context
    }
    mem_use_t *free_row = NULL;
    for (int i = 0; i < MEM_BOOK_OWNERS; i++) {
        mem_use_t *r = &b->row[i];
        if (!r->ctx) {
            free_row = free_row ? free_row : r;
        } else if (r->ctx == ctx) {
            return r;
        }
    }
    if (make && free_row) {
        memset(free_row, 0, sizeof(*free_row));
        free_row->ctx = ctx;
        free_row->os  = os;
        owner_fit(free_row->owner, label);
        return free_row;
    }
    return NULL;
}

static void add(mem_use_t *r, bool internal, uint32_t bytes) {
    if (internal) {
        r->internal += bytes;
        r->internal_peak = r->internal > r->internal_peak ? r->internal : r->internal_peak;
    } else {
        r->bulk += bytes;
        r->bulk_peak = r->bulk > r->bulk_peak ? r->bulk : r->bulk_peak;
    }
}

mem_charge_t mem_book_charge(mem_book_t *b, uint32_t ctx, char const *label, bool os, bool internal, uint32_t bytes) {
    mem_use_t *r = row_of(b, ctx, true, label, os);
    if (!r) {
        return MEM_CHARGE_NO_ROW;
    }
    if (internal && !r->os && (bytes > MEM_BOOK_INTERNAL_QUOTA || r->internal > MEM_BOOK_INTERNAL_QUOTA - bytes)) {
        r->refused++;
        return MEM_CHARGE_QUOTA;
    }
    add(r, internal, bytes);
    r->blocks++;
    return MEM_CHARGE_OK;
}

void mem_book_adjust(mem_book_t *b, uint32_t ctx, bool internal, uint32_t extra) {
    mem_use_t *r = row_of(b, ctx, false, NULL, false);
    if (r) {
        add(r, internal, extra);
    }
}

mem_release_t mem_book_release(mem_book_t *b, uint32_t ctx, bool internal, uint32_t bytes) {
    mem_use_t *r = row_of(b, ctx, false, NULL, false);
    if (!r) {
        return MEM_RELEASE_NO_ROW;
    }
    uint32_t *held = internal ? &r->internal : &r->bulk;
    bool      over = bytes > *held || !r->blocks;
    *held          = over ? 0 : *held - bytes;
    if (r->blocks) {
        r->blocks--;
    }
    return over ? MEM_RELEASE_OVER : MEM_RELEASE_OK;
}

mem_use_t const *mem_book_find(mem_book_t const *b, uint32_t ctx) {
    return row_of((mem_book_t *)b, ctx, false, NULL, false);
}
