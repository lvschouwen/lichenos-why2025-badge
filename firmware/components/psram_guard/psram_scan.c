#include "psram_scan.h"

#include "psram_pattern.h"
#include "psram_learned.h"
#include "psram_quar.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define CHUNK       (64u * 1024u)  // scan and grab unit
#define MAX_HELD    256
#define MAX_CHUNKS  1024
#define WIN_LO      0x48400000u // the suspect window of #40: bad bytes near 0x4855BA81, 4 MB check ended at 0x485C0A80
#define WIN_HI      0x48A00000u
#define LINE        128
#define MAX_ROUNDS  8              // extra rounds over the learned range +- NEAR: the errors are intermittent, more writes find more
#define BUDGET_US   2800000        // the whole scan stays within about this (Lucas: 2-3 s of boot is fine)
#define NEAR        (448u * 1024u) // chunks this close to the quarantine (learned range + 64 KB margin) get the extra rounds
#define PSRAM_CAPS  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SPARSE_XOR  0xAAAAAAAAu // BadgeVMS' boot test: every 8th word of the PSRAM, address ^ this
#define MAX_REGIONS 6
#define PASSES      4           // 0 sparse, 1 dense, 2 dense inverted, 3 dense again

static char const TAG[] = "psram";

typedef struct {
    uint8_t *mem;
    size_t   len;
    size_t   bad[PASSES];
} chunk_t;

typedef struct {
    char const *name;
    uintptr_t   addr;
    size_t      len;
} region_t;

static chunk_t       s_chunks[MAX_CHUNKS]; // every chunk grabbed, sorted by address
static size_t        s_n;
static uint8_t      *s_far[MAX_CHUNKS];     // the rest of the free PSRAM while the edges are split
static size_t        s_nfar;
static chunk_t       s_held[MAX_HELD];    // kept for good (quarantine)
static size_t        s_nheld;
static psram_quar_t  s_quar;
static char          s_map[200];
static region_t      s_regions[MAX_REGIONS];
static size_t        s_nregions;
static struct {
    size_t   scanned, bad_chunks, pass_bad[PASSES], bad_bytes;
    uint32_t xor_or, up_or, down_or;
    int64_t  us;
    uintptr_t lowest, highest;
    size_t   gaps_in_window, rounds, short_pages, pages_held;
    uintptr_t find_lo, find_hi; // lowest / highest bad byte of this scan
} s_stat;

static int by_addr(void const *a, void const *b) {
    uintptr_t x = (uintptr_t)((chunk_t const *)a)->mem, y = (uintptr_t)((chunk_t const *)b)->mem;
    return x < y ? -1 : x > y;
}

static void sync(uint8_t *mem, size_t len) {
    esp_cache_msync(mem, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

// BadgeVMS' pattern on one chunk: every 8th word = its address ^ 0xAAAAAAAA. Returns bad words; records bits/extent.
static size_t sparse_pass(uint8_t *mem, psram_check_t *acc) {
    uint32_t *w = (uint32_t *)mem;
    size_t    n = CHUNK / 4;
    for (size_t i = 0; i < n; i += 8) {
        w[i] = (uint32_t)(uintptr_t)&w[i] ^ SPARSE_XOR;
    }
    sync(mem, CHUNK);
    size_t bad = 0;
    for (size_t i = 0; i < n; i += 8) {
        uint32_t want = (uint32_t)(uintptr_t)&w[i] ^ SPARSE_XOR;
        uint32_t diff = w[i] ^ want;
        if (!diff) {
            continue;
        }
        psram_learned_mark((uintptr_t)&w[i]);
        if (bad++ == 0 && !acc->bad_bytes) {
            acc->first_bad = i * 4;
        }
        acc->bad_bytes++;
        acc->last_bad = i * 4 + 3;
        acc->xor_or  |= diff;
        acc->up_or   |= w[i] & ~want;
        acc->down_or |= want & ~w[i];
    }
    return bad;
}

static void dense_pass(uint8_t *mem, uint32_t mask, psram_check_t *out) {
    psram_pattern_fill_x(mem, CHUNK, (uintptr_t)mem, mask);
    sync(mem, CHUNK);
    *out = psram_pattern_check_x(mem, CHUNK, (uintptr_t)mem, mask);
    if (out->bad_bytes) {
        psram_learned_mark_buf(mem, CHUNK, (uintptr_t)mem, mask, out->first_bad, out->last_bad);
    }
}

// Folds one pass result into the chunk's extent and the global bit statistics
static void fold(chunk_t *c, int pass, psram_check_t const *r, size_t *first, size_t *last) {
    if (!r->bad_bytes) {
        return;
    }
    c->bad[pass] += r->bad_bytes;
    *first = r->first_bad < *first ? r->first_bad : *first;
    *last  = r->last_bad > *last ? r->last_bad : *last;
    s_stat.pass_bad[pass] += r->bad_bytes;
    s_stat.xor_or  |= r->xor_or;
    s_stat.up_or   |= r->up_or;
    s_stat.down_or |= r->down_or;
}

static void scan_chunk(chunk_t *c) {
    psram_check_t r = {0};
    size_t        first = SIZE_MAX, last = 0;
    size_t before = c->bad[0] + c->bad[1] + c->bad[2] + c->bad[3];
    sparse_pass(c->mem, &r);
    fold(c, 0, &r, &first, &last);
    static uint32_t const masks[3] = {0, 0xFFFFFFFFu, 0};
    for (int p = 0; p < 3; p++) {
        dense_pass(c->mem, masks[p], &r);
        fold(c, p + 1, &r, &first, &last);
    }
    s_stat.scanned++;
    size_t total = c->bad[0] + c->bad[1] + c->bad[2] + c->bad[3] - before;
    if (total) {
        s_stat.bad_chunks += before == 0;
        s_stat.bad_bytes += total;
        uintptr_t flo = (uintptr_t)c->mem + first, fhi = (uintptr_t)c->mem + last;
        s_stat.find_lo = !s_stat.find_hi || flo < s_stat.find_lo ? flo : s_stat.find_lo;
        s_stat.find_hi = fhi > s_stat.find_hi ? fhi : s_stat.find_hi;
        uint32_t bits = s_stat.xor_or;
        psram_quar_note(&s_quar, (uintptr_t)c->mem + first, (uintptr_t)c->mem + last, (uint32_t)total, bits);
        ESP_LOGW(TAG, "scan: chunk %p bad: sparse %u, dense %u / %u / %u bytes, extent +%u..+%u", (void *)c->mem,
                 (unsigned)c->bad[0], (unsigned)c->bad[1], (unsigned)c->bad[2], (unsigned)c->bad[3], (unsigned)first,
                 (unsigned)last);
    }
}

static void grab_all(void) {
    while (s_n < MAX_CHUNKS) {
        uint8_t *m = heap_caps_aligned_alloc(LINE, CHUNK, PSRAM_CAPS);
        if (!m) {
            break;
        }
        s_chunks[s_n++] = (chunk_t){.mem = m, .len = CHUNK};
    }
    qsort(s_chunks, s_n, sizeof(chunk_t), by_addr);
}

static bool in_window(chunk_t const *c) {
    uintptr_t a = (uintptr_t)c->mem;
    return a + CHUNK > WIN_LO && a < WIN_HI;
}

// Gaps between neighbouring chunks inside the window are memory somebody else already owns (code, rodata, early heap)
static void log_map(void) {
    extern int _instruction_reserved_start, _instruction_reserved_end, _rodata_reserved_start, _rodata_reserved_end;
    s_stat.lowest  = s_n ? (uintptr_t)s_chunks[0].mem : 0;
    s_stat.highest = s_n ? (uintptr_t)s_chunks[s_n - 1].mem + CHUNK : 0;
    snprintf(s_map, sizeof(s_map), "%u chunks of 64 KB free at boot, %p..%p; text %p..%p, rodata %p..%p, framebuffer: see log",
             (unsigned)s_n, (void *)s_stat.lowest, (void *)s_stat.highest, (void *)&_instruction_reserved_start,
             (void *)&_instruction_reserved_end, (void *)&_rodata_reserved_start, (void *)&_rodata_reserved_end);
    ESP_LOGW(TAG, "map: %u chunks of 64 KB grabbed from %p to %p, %u KB free PSRAM left; text %p..%p, rodata %p..%p",
             (unsigned)s_n, (void *)s_stat.lowest, (void *)s_stat.highest,
             (unsigned)(heap_caps_get_free_size(PSRAM_CAPS) >> 10), (void *)&_instruction_reserved_start,
             (void *)&_instruction_reserved_end, (void *)&_rodata_reserved_start, (void *)&_rodata_reserved_end);
    for (size_t i = 1; i < s_n; i++) {
        uintptr_t end = (uintptr_t)s_chunks[i - 1].mem + CHUNK, next = (uintptr_t)s_chunks[i].mem;
        if (next - end > 4096) {
            if (next > WIN_LO && end < WIN_HI) {
                s_stat.gaps_in_window++;
            }
            ESP_LOGW(TAG, "map: in use %p..%p (%u KB)%s", (void *)end, (void *)next, (unsigned)((next - end) >> 10),
                     next > WIN_LO && end < WIN_HI ? " in the scan window" : "");
        }
    }
}

static bool inside_quar(uintptr_t a, size_t len) {
    for (size_t i = 0; i < s_quar.n; i++) {
        if (a >= s_quar.r[i].lo && a + len <= s_quar.r[i].hi) {
            return true;
        }
    }
    return false;
}

static void hold(uint8_t *mem, size_t len, chunk_t const *from) {
    if (s_nheld < MAX_HELD) {
        s_held[s_nheld]     = from ? *from : (chunk_t){0};
        s_held[s_nheld].mem = mem;
        s_held[s_nheld].len = len;
        s_nheld++;
    } else {
        heap_caps_free(mem);
    }
}

// The intersection of a chunk with the quarantine ranges it touches (their hull)
static void touched(uintptr_t base, size_t len, uintptr_t *lo, uintptr_t *hi) {
    *lo = UINTPTR_MAX;
    *hi = 0;
    for (size_t i = 0; i < s_quar.n; i++) {
        if (base < s_quar.r[i].hi && base + len > s_quar.r[i].lo) {
            uintptr_t l = s_quar.r[i].lo > base ? s_quar.r[i].lo : base;
            uintptr_t h = s_quar.r[i].hi < base + len ? s_quar.r[i].hi : base + len;
            *lo         = l < *lo ? l : *lo;
            *hi         = h > *hi ? h : *hi;
        }
    }
}

// A chunk that only touches the quarantine at its edge: give it back and claim just the part inside the range. With
// everything else PSRAM held (drop_far is deferred) the freed block is the only free memory, so a filler allocation
// takes its start and the wanted piece follows it. The slop is the heap header and the 128 B alignment: the piece
// starts at or before the range and ends at or after it; if the heap hands something else, the retry widens the slop
// and a failure is counted ("pages missing") instead of hidden.
static void split_edge(chunk_t const *c) {
    uintptr_t base = (uintptr_t)c->mem, lo, hi;
    touched(base, CHUNK, &lo, &hi);
    heap_caps_free(c->mem);
    static size_t const slop[3] = {192, 512, 2048};
    for (int attempt = 0; attempt < 3; attempt++) {
        size_t   fill = lo - base > slop[attempt] ? lo - base - slop[attempt] : 0;
        uint8_t *f    = fill ? heap_caps_aligned_alloc(LINE, fill, PSRAM_CAPS) : NULL;
        size_t   want = (hi + slop[attempt] - (base + fill) + LINE - 1) & ~(size_t)(LINE - 1);
        uint8_t *m    = heap_caps_aligned_alloc(LINE, want, PSRAM_CAPS);
        if (f) {
            heap_caps_free(f);
        }
        if (m && (uintptr_t)m <= lo && (uintptr_t)m + want >= hi) {
            hold(m, want, c);
            s_stat.pages_held++;
            return;
        }
        heap_caps_free(m); // free(NULL) is fine
    }
    s_stat.short_pages++;
}

// Everything still free is taken for the moment, so a freed edge chunk is the only free block (split_edge)
static void grab_rest(void) {
    while (s_nfar < MAX_CHUNKS) {
        uint8_t *m = heap_caps_aligned_alloc(LINE, CHUNK, PSRAM_CAPS);
        if (!m) {
            break;
        }
        s_far[s_nfar++] = m;
    }
}

static void keep_or_free(void) {
    grab_rest();
    for (size_t i = 0; i < s_n; i++) { // first the ones that stay, so a freed neighbour never merges into an edge block
        uintptr_t a = (uintptr_t)s_chunks[i].mem;
        if (inside_quar(a, CHUNK)) {
            hold(s_chunks[i].mem, CHUNK, &s_chunks[i]);
            s_chunks[i].mem = NULL;
        } else if (psram_quar_overlaps(&s_quar, a, CHUNK)) {
            split_edge(&s_chunks[i]);
            s_chunks[i].mem = NULL;
        }
    }
    for (size_t i = 0; i < s_n; i++) {
        heap_caps_free(s_chunks[i].mem);
    }
    for (size_t i = 0; i < s_nfar; i++) {
        heap_caps_free(s_far[i]);
    }
    s_nfar = 0;
}

// Chunks well outside the window go back at once, so the rest of the boot is not starved while the window is scanned
static void drop_far(void) {
    size_t kept = 0;
    for (size_t i = 0; i < s_n; i++) {
        uintptr_t a = (uintptr_t)s_chunks[i].mem;
        if (a + 4 * CHUNK > WIN_LO && a < WIN_HI + 4 * CHUNK) {
            s_chunks[kept++] = s_chunks[i];
        } else {
            heap_caps_free(s_chunks[i].mem);
        }
    }
    s_n = kept;
}

static bool near_quar(chunk_t const *c) {
    uintptr_t a = (uintptr_t)c->mem;
    for (size_t k = 0; k < s_quar.n; k++) {
        if (a + CHUNK + NEAR > s_quar.r[k].lo && a < s_quar.r[k].hi + NEAR) {
            return true;
        }
    }
    return false;
}

// The failures are intermittent and thin out at the edges: rewrite the zone around the quarantine as often as the
// time budget allows, so a rare byte beyond the learned edge still shows up and widens the range.
static void extra_rounds(int64_t start) {
    int64_t last = 0;
    for (int round = 0; round < MAX_ROUNDS && s_quar.n; round++) {
        int64_t t0 = esp_timer_get_time();
        if (t0 - start + last > BUDGET_US) {
            break;
        }
        for (size_t i = 0; i < s_n; i++) {
            if (near_quar(&s_chunks[i])) {
                scan_chunk(&s_chunks[i]);
            }
        }
        last = esp_timer_get_time() - t0;
        s_stat.rounds++;
    }
}

void psram_scan_run(void) {
    int64_t start = esp_timer_get_time();
    size_t  free0 = heap_caps_get_free_size(PSRAM_CAPS);
    psram_learned_load();
    psram_learn_t lr = psram_learned_get();
    if (!psram_learn_empty(&lr)) { // quarantine what was learned before looking at anything
        psram_quar_note(&s_quar, lr.lo, lr.hi, 0, 0);
    }
    grab_all();
    log_map();
    drop_far();
    for (size_t i = 0; i < s_n; i++) {
        if (in_window(&s_chunks[i])) {
            scan_chunk(&s_chunks[i]);
        }
    }
    extra_rounds(start);
    if (s_stat.find_hi) {
        psram_learned_feed(s_stat.find_lo, s_stat.find_hi, "boot scan");
    }
    keep_or_free();
    s_stat.us = esp_timer_get_time() - start;
    ESP_LOGW(TAG, "scan: window %p..%p, %u chunk scans + %u extra rounds in %u ms, %u bad chunks, bad bytes sparse %u dense %u/%u/%u; "
                  "flipped bits %08x (0->1 %08x, 1->0 %08x); quarantine %u range(s) %u KB, held %u pieces (%u 4K edge pages, %u missing), free PSRAM %u -> %u KB",
             (void *)WIN_LO, (void *)WIN_HI, (unsigned)s_stat.scanned, (unsigned)s_stat.rounds, (unsigned)(s_stat.us / 1000),
             (unsigned)s_stat.bad_chunks, (unsigned)s_stat.pass_bad[0], (unsigned)s_stat.pass_bad[1], (unsigned)s_stat.pass_bad[2],
             (unsigned)s_stat.pass_bad[3], (unsigned)s_stat.xor_or, (unsigned)s_stat.up_or, (unsigned)s_stat.down_or,
             (unsigned)s_quar.n, (unsigned)(psram_quar_bytes(&s_quar) >> 10), (unsigned)s_nheld, (unsigned)s_stat.pages_held,
             (unsigned)s_stat.short_pages, (unsigned)(free0 >> 10), (unsigned)(heap_caps_get_free_size(PSRAM_CAPS) >> 10));
}

void psram_scan_note_region(char const *name, void const *addr, size_t len) {
    if (s_nregions < MAX_REGIONS) {
        s_regions[s_nregions++] = (region_t){name, (uintptr_t)addr, len};
    }
}

void psram_scan_print(void) {
    console_reply("psram scan: window %p..%p, %u chunk scans + %u extra rounds in %u ms, %u bad chunks, bad bytes sparse %u dense %u/%u/%u, "
                  "bits %08x (0->1 %08x, 1->0 %08x), %u gaps (memory in use) in the window",
                  (void *)WIN_LO, (void *)WIN_HI, (unsigned)s_stat.scanned, (unsigned)s_stat.rounds, (unsigned)(s_stat.us / 1000),
                  (unsigned)s_stat.bad_chunks, (unsigned)s_stat.pass_bad[0], (unsigned)s_stat.pass_bad[1],
                  (unsigned)s_stat.pass_bad[2], (unsigned)s_stat.pass_bad[3], (unsigned)s_stat.xor_or,
                  (unsigned)s_stat.up_or, (unsigned)s_stat.down_or, (unsigned)s_stat.gaps_in_window);
    if (!s_quar.n) {
        console_reply("psram quarantine: none (no bad bytes in the window)");
    }
    for (size_t i = 0; i < s_quar.n; i++) {
        psram_quar_range_t const *r = &s_quar.r[i];
        console_reply("psram quarantine %u: %p..%p (%u KB), bad bytes %p..%p (%u seen), bits %08x", (unsigned)i,
                      (void *)r->lo, (void *)r->hi, (unsigned)((r->hi - r->lo) >> 10), (void *)r->first_bad,
                      (void *)r->last_bad, (unsigned)r->bad_bytes, (unsigned)r->bits);
    }
    size_t held = 0;
    for (size_t i = 0; i < s_nheld; i++) {
        held += s_held[i].len;
    }
    psram_learned_print();
    console_reply("psram quarantine: %u KB in ranges, %u KB held in %u pieces (%u edge pieces, %u missing)",
                  (unsigned)(psram_quar_bytes(&s_quar) >> 10), (unsigned)(held >> 10), (unsigned)s_nheld,
                  (unsigned)s_stat.pages_held, (unsigned)s_stat.short_pages);
    for (size_t i = 0; i < s_nheld; i++) {
        size_t t = s_held[i].bad[0] + s_held[i].bad[1] + s_held[i].bad[2] + s_held[i].bad[3];
        if (t) {
            console_reply("psram scan chunk %p: bad bytes sparse %u dense %u/%u/%u", (void *)s_held[i].mem,
                          (unsigned)s_held[i].bad[0], (unsigned)s_held[i].bad[1], (unsigned)s_held[i].bad[2],
                          (unsigned)s_held[i].bad[3]);
        }
    }
    console_reply("psram map: %s", s_map);
    for (size_t i = 0; i < s_nregions; i++) {
        region_t const *g = &s_regions[i];
        console_reply("psram region %s %p..%p%s", g->name, (void *)g->addr, (void *)(g->addr + g->len),
                      psram_quar_overlaps(&s_quar, g->addr, g->len) ? " OVERLAPS the quarantine" : "");
    }
}

#define WATCH_MAX 128

typedef struct {
    uintptr_t at[WATCH_MAX]; // first bad byte of each bad 128 B line
    size_t    n, bytes;
    uint32_t  up, down;
    uintptr_t lo, hi; // extent of the bad bytes
} watch_t;

static void watch_read(watch_t *w) {
    *w = (watch_t){0};
    for (size_t i = 0; i < s_nheld; i++) {
        uint8_t *m = s_held[i].mem;
        sync(m, s_held[i].len);
        for (size_t off = 0; off < s_held[i].len; off += LINE) {
            psram_check_t r = psram_pattern_check_x(m + off, LINE, (uintptr_t)m + off, 0);
            if (!r.bad_bytes) {
                continue;
            }
            if (w->n < WATCH_MAX) {
                w->at[w->n] = (uintptr_t)m + off + r.first_bad;
            }
            uintptr_t blo = (uintptr_t)m + off + r.first_bad, bhi = (uintptr_t)m + off + r.last_bad;
            w->lo = !w->hi || blo < w->lo ? blo : w->lo;
            w->hi = bhi > w->hi ? bhi : w->hi;
            psram_learned_mark(blo);
            psram_learned_mark(bhi);
            w->n++;
            w->bytes += r.bad_bytes;
            w->up    |= r.up_or;
            w->down  |= r.down_or;
        }
    }
}

static size_t common(watch_t const *a, watch_t const *b) {
    size_t same = 0;
    for (size_t i = 0; i < a->n && i < WATCH_MAX; i++) {
        for (size_t j = 0; j < b->n && j < WATCH_MAX; j++) {
            same += a->at[i] == b->at[j];
        }
    }
    return same;
}

static size_t held_bytes(void) {
    size_t sum = 0;
    for (size_t i = 0; i < s_nheld; i++) {
        sum += s_held[i].len;
    }
    return sum;
}

static void watch_line(char const *when, watch_t const *w) {
    console_reply("psram qwatch %s: %u bad lines, %u bad bytes, 0->1 %08x, 1->0 %08x", when, (unsigned)w->n, (unsigned)w->bytes,
                  (unsigned)w->up, (unsigned)w->down);
}

void psram_scan_watch(unsigned ms) {
    if (!s_nheld) {
        console_reply("psram qwatch: nothing held");
        return;
    }
    static watch_t a, b, c;
    for (size_t i = 0; i < s_nheld; i++) {
        psram_pattern_fill_x(s_held[i].mem, s_held[i].len, (uintptr_t)s_held[i].mem, 0);
        sync(s_held[i].mem, s_held[i].len);
    }
    watch_read(&a); // straight after the write
    vTaskDelay(pdMS_TO_TICKS(ms));
    watch_read(&b); // after the wait
    watch_read(&c); // and once more: the same lines again = stored wrong, other lines = read errors
    watch_t const *all[3] = {&a, &b, &c};
    for (int i = 0; i < 3; i++) {
        if (all[i]->hi) {
            psram_learned_feed(all[i]->lo, all[i]->hi, "qwatch");
        }
    }
    watch_line("right after the write", &a);
    watch_line("after the wait", &b);
    watch_line("read again", &c);
    for (size_t i = 0; i < b.n && i < 8; i++) {
        console_reply("psram qwatch bad byte %p (offset in line %u)", (void *)b.at[i], (unsigned)(b.at[i] & (LINE - 1)));
    }
    console_reply("psram qwatch: %u chunks (%u KB), wait %u ms; same first-bad byte in write/wait %u, wait/again %u of %u",
                  (unsigned)s_nheld, (unsigned)(held_bytes() >> 10), ms, (unsigned)common(&a, &b), (unsigned)common(&b, &c),
                  (unsigned)b.n);
}
