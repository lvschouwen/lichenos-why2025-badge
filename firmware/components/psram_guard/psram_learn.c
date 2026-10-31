#include "psram_learn.h"

void psram_learn_reset(psram_learn_t *r, unsigned speed_mhz) {
    *r = (psram_learn_t){.version = PSRAM_LEARN_VERSION, .speed_mhz = (uint16_t)speed_mhz, .lo = UINT32_MAX, .hi = 0};
}

bool psram_learn_empty(psram_learn_t const *r) {
    return r->lo > r->hi;
}

static bool page_index(uintptr_t addr, unsigned *idx);

static bool in_psram(uintptr_t a) {
    return a >= PSRAM_ADDR_MIN && a < PSRAM_ADDR_END;
}

bool psram_learn_usable(psram_learn_t const *r, size_t size, unsigned speed_mhz) {
    if (size != sizeof(*r) || r->version != PSRAM_LEARN_VERSION || r->speed_mhz != speed_mhz) {
        return false;
    }
    return psram_learn_empty(r) || (in_psram(r->lo) && in_psram(r->hi));
}

psram_learn_result_t psram_learn_note(psram_learn_t *r, uintptr_t lo, uintptr_t hi) {
    if (lo > hi || !in_psram(lo) || !in_psram(hi)) {
        return PSRAM_LEARN_OUTLIER;
    }
    uintptr_t nlo = psram_learn_empty(r) || lo < r->lo ? lo : r->lo;
    uintptr_t nhi = psram_learn_empty(r) || hi > r->hi ? hi : r->hi;
    bool paged = psram_learn_mark_page(r, lo);
    paged |= psram_learn_mark_page(r, hi);
    if (!psram_learn_empty(r) && nlo == r->lo && nhi == r->hi) {
        if (!paged) {
            return PSRAM_LEARN_SAME;
        }
        r->writes++;
        return PSRAM_LEARN_GREW;
    }
    r->lo = (uint32_t)nlo;
    r->hi = (uint32_t)nhi;
    r->writes++;
    return PSRAM_LEARN_GREW;
}

static bool page_index(uintptr_t addr, unsigned *idx) {
    if (addr < PSRAM_MAP_BASE || addr >= PSRAM_MAP_BASE + PSRAM_MAP_PAGES * PSRAM_PAGE) {
        return false;
    }
    *idx = (unsigned)((addr - PSRAM_MAP_BASE) / PSRAM_PAGE);
    return true;
}

bool psram_learn_mark_page(psram_learn_t *r, uintptr_t addr) {
    unsigned i;
    if (!page_index(addr, &i) || (r->pages[i / 8] >> (i % 8) & 1)) {
        return false;
    }
    r->pages[i / 8] |= (uint8_t)(1u << (i % 8));
    return true;
}

void psram_learn_page_counts(psram_learn_t const *r, unsigned *span_pages, unsigned *failed) {
    *span_pages = 0;
    *failed     = 0;
    if (psram_learn_empty(r)) {
        return;
    }
    uintptr_t first = r->lo & ~(uintptr_t)(PSRAM_PAGE - 1);
    for (uintptr_t a = first; a <= r->hi; a += PSRAM_PAGE) {
        unsigned i;
        (*span_pages)++;
        if (page_index(a, &i) && (r->pages[i / 8] >> (i % 8) & 1)) {
            (*failed)++;
        }
    }
}

size_t psram_learn_page_map(psram_learn_t const *r, char *out, size_t cap) {
    size_t n = 0;
    if (!psram_learn_empty(r)) {
        for (uintptr_t a = r->lo & ~(uintptr_t)(PSRAM_PAGE - 1); a <= r->hi && n + 1 < cap; a += PSRAM_PAGE) {
            unsigned i;
            out[n++] = page_index(a, &i) && (r->pages[i / 8] >> (i % 8) & 1) ? '#' : '.';
        }
    }
    if (cap) {
        out[n] = 0;
    }
    return n;
}

bool psram_learn_seed(psram_learn_t *r) {
    if (psram_learn_empty(r)) {
        return false;
    }
    bool changed = psram_learn_mark_page(r, r->lo);
    changed |= psram_learn_mark_page(r, r->hi);
    return changed;
}

bool psram_learn_next_failed(psram_learn_t const *r, uintptr_t from, uintptr_t *page) {
    uintptr_t a = from < PSRAM_MAP_BASE ? PSRAM_MAP_BASE : from & ~(uintptr_t)(PSRAM_PAGE - 1);
    for (unsigned i; page_index(a, &i); a += PSRAM_PAGE) {
        if (r->pages[i / 8] >> (i % 8) & 1) {
            *page = a;
            return true;
        }
    }
    return false;
}

unsigned psram_learn_failed_pages(psram_learn_t const *r) {
    unsigned n = 0;
    for (unsigned i = 0; i < PSRAM_MAP_PAGES; i++) {
        n += r->pages[i / 8] >> (i % 8) & 1;
    }
    return n;
}
