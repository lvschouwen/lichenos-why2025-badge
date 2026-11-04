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

static mem_use_t *row_of(mem_book_t *b, char const *owner, bool make) {
    char want[MEM_BOOK_OWNER_CAP];
    owner_fit(want, owner);
    mem_use_t *free_row = NULL;
    for (int i = 0; i < MEM_BOOK_OWNERS; i++) {
        mem_use_t *r = &b->row[i];
        if (!r->owner[0]) {
            free_row = free_row ? free_row : r;
        } else if (strcmp(r->owner, want) == 0) {
            return r;
        }
    }
    if (make && free_row) {
        memset(free_row, 0, sizeof(*free_row));
        memcpy(free_row->owner, want, sizeof(want));
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

bool mem_book_charge(mem_book_t *b, char const *owner, bool internal, uint32_t bytes) {
    mem_use_t *r = row_of(b, owner, true);
    if (!r) {
        return false;
    }
    bool limited = strcmp(r->owner, MEM_BOOK_OS) != 0;
    if (internal && limited && (bytes > MEM_BOOK_INTERNAL_QUOTA || r->internal > MEM_BOOK_INTERNAL_QUOTA - bytes)) {
        r->refused++;
        return false;
    }
    add(r, internal, bytes);
    r->blocks++;
    return true;
}

void mem_book_adjust(mem_book_t *b, char const *owner, bool internal, uint32_t extra) {
    mem_use_t *r = row_of(b, owner, false);
    if (r) {
        add(r, internal, extra);
    }
}

void mem_book_release(mem_book_t *b, char const *owner, bool internal, uint32_t bytes) {
    mem_use_t *r = row_of(b, owner, false);
    if (!r) {
        return;
    }
    uint32_t *held = internal ? &r->internal : &r->bulk;
    *held          = *held > bytes ? *held - bytes : 0;
    if (r->blocks) {
        r->blocks--;
    }
}

mem_use_t const *mem_book_find(mem_book_t const *b, char const *owner) {
    return row_of((mem_book_t *)b, owner, false);
}
