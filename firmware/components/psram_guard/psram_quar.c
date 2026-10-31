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

size_t psram_quar_uncovered(psram_quar_t const *q, psram_span_t const *spans, size_t n, size_t slack) {
    size_t missing = 0;
    for (size_t i = 0; i < q->n; i++) {
        uintptr_t cur = q->r[i].lo, hi = q->r[i].hi;
        for (size_t k = 0; k < n && cur < hi; k++) {
            if (spans[k].hi <= cur) {
                continue;
            }
            if (spans[k].lo >= hi) {
                break;
            }
            if (spans[k].lo > cur + slack) {
                missing += spans[k].lo - cur;
            }
            cur = spans[k].hi > cur ? spans[k].hi : cur;
        }
        if (cur < hi && hi - cur > slack) {
            missing += hi - cur;
        }
    }
    return missing;
}
