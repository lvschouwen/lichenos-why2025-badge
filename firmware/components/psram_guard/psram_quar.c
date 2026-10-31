#include "psram_quar.h"

static uintptr_t down(uintptr_t a) {
    return a & ~(uintptr_t)(PSRAM_QUAR_GRAIN - 1);
}

static uintptr_t up(uintptr_t a) {
    return down(a + PSRAM_QUAR_GRAIN - 1);
}

static void merge_into(psram_quar_range_t *a, psram_quar_range_t const *b) {
    a->lo        = b->lo < a->lo ? b->lo : a->lo;
    a->hi        = b->hi > a->hi ? b->hi : a->hi;
    a->first_bad = b->first_bad < a->first_bad ? b->first_bad : a->first_bad;
    a->last_bad  = b->last_bad > a->last_bad ? b->last_bad : a->last_bad;
    a->bits     |= b->bits;
    a->bad_bytes += b->bad_bytes;
}

static void remove_at(psram_quar_t *q, size_t i) {
    for (; i + 1 < q->n; i++) {
        q->r[i] = q->r[i + 1];
    }
    q->n--;
}

// Merge ranges that touch or overlap
static void tidy(psram_quar_t *q) {
    for (size_t i = 0; i + 1 < q->n;) {
        if (q->r[i + 1].lo <= q->r[i].hi) {
            merge_into(&q->r[i], &q->r[i + 1]);
            remove_at(q, i + 1);
        } else {
            i++;
        }
    }
}

// More ranges than slots: join the pair with the smallest gap between them
static void squeeze(psram_quar_t *q) {
    while (q->n > PSRAM_QUAR_MAX) {
        size_t best = 0;
        for (size_t i = 1; i + 1 < q->n; i++) {
            if (q->r[i + 1].lo - q->r[i].hi < q->r[best + 1].lo - q->r[best].hi) {
                best = i;
            }
        }
        merge_into(&q->r[best], &q->r[best + 1]);
        remove_at(q, best + 1);
    }
}

void psram_quar_note(psram_quar_t *q, uintptr_t first_bad, uintptr_t last_bad, uint32_t bad_bytes, uint32_t bits) {
    psram_quar_range_t nr = {
        .lo        = down(first_bad > PSRAM_QUAR_MARGIN ? first_bad - PSRAM_QUAR_MARGIN : 0),
        .hi        = up(last_bad + 1 + PSRAM_QUAR_MARGIN),
        .first_bad = first_bad,
        .last_bad  = last_bad,
        .bits      = bits,
        .bad_bytes = bad_bytes,
    };
    size_t i = q->n; // one slot of slack: the table holds MAX + 1 while squeezing
    q->r[i]  = nr;
    q->n++;
    while (i > 0 && q->r[i - 1].lo > q->r[i].lo) {
        psram_quar_range_t t = q->r[i - 1];
        q->r[i - 1]          = q->r[i];
        q->r[i]              = t;
        i--;
    }
    tidy(q);
    squeeze(q);
}

bool psram_quar_overlaps(psram_quar_t const *q, uintptr_t addr, size_t len) {
    for (size_t i = 0; i < q->n; i++) {
        if (addr < q->r[i].hi && addr + len > q->r[i].lo) {
            return true;
        }
    }
    return false;
}

size_t psram_quar_bytes(psram_quar_t const *q) {
    size_t sum = 0;
    for (size_t i = 0; i < q->n; i++) {
        sum += q->r[i].hi - q->r[i].lo;
    }
    return sum;
}

size_t psram_quar_cut(psram_quar_t const *q, uintptr_t start, uintptr_t end, size_t min, psram_quar_piece_t *out, size_t max) {
    size_t    n  = 0;
    uintptr_t at = start;
    for (size_t i = 0; i < q->n && at < end; i++) {
        if (q->r[i].hi <= at) {
            continue;
        }
        if (q->r[i].lo >= end) {
            break;
        }
        if (q->r[i].lo > at && q->r[i].lo - at >= min && n < max) {
            out[n++] = (psram_quar_piece_t){at, q->r[i].lo};
        }
        at = q->r[i].hi > at ? q->r[i].hi : at;
    }
    // The last piece: an uncut pool (nothing quarantined in it) is kept whatever its size, a cut remainder only if long enough
    if (at < end && (at == start || end - at >= min) && n < max) {
        out[n++] = (psram_quar_piece_t){at, end};
    }
    return n;
}

static bool in_pieces(psram_quar_piece_t const *p, size_t n, uintptr_t a) {
    for (size_t i = 0; i < n; i++) {
        if (a >= p[i].lo && a < p[i].hi) {
            return true;
        }
    }
    return false;
}

bool psram_quar_in_heap(psram_quar_piece_t const *pools, size_t npools, psram_quar_piece_t const *holes, size_t nholes,
                        uintptr_t p) {
    return in_pieces(pools, npools, p) && !in_pieces(holes, nholes, p);
}

int psram_quar_flip_bit(psram_quar_piece_t const *pools, size_t npools, psram_quar_piece_t const *holes, size_t nholes,
                        uintptr_t p) {
    for (int b = 0; b < (int)(8 * sizeof(uintptr_t)); b++) {
        if (psram_quar_in_heap(pools, npools, holes, nholes, p ^ ((uintptr_t)1 << b))) {
            return b;
        }
    }
    return -1;
}
