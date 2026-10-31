#include "psram_scan.h"
#include "psram_guard.h"

#include "psram_pattern.h"
#include "psram_learned.h"
#include "psram_quar.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define CHUNK       (64u * 1024u)  // scan and grab unit
#define MAX_HELD    64             // chunks held until reboot: the ones where this boot found new bad bytes
#define MAX_CHUNKS  1024
#define MAX_POOLS   8              // heap regions the IDF registers for the PSRAM (the pool + the irom/drom alignment gaps)
#define MAX_PIECES  (PSRAM_QUAR_MAX + MAX_POOLS)
#define MAX_HOLES   MAX_PIECES
#define WIN_LO      0x48400000u // the suspect window of #40: bad bytes near 0x4855BA81, 4 MB check ended at 0x485C0A80
#define WIN_HI      0x48A00000u
#define LINE        128
#define MAX_ROUNDS  8              // extra rounds over the learned range +- NEAR: the errors are intermittent, more writes find more
#define BUDGET_US   2800000        // the whole scan stays within about this (Lucas: 2-3 s of boot is fine)
#define NEAR        (448u * 1024u) // chunks this close to the quarantine (failed pages + 4 KB margin) get the extra rounds
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

typedef struct {
    uint32_t caps[SOC_MEMORY_TYPE_NO_PRIOS];
    intptr_t start, end;
} pool_t;

typedef struct {
    uint8_t *mem;
    size_t   len;
} own_t; // memory only this module touches: a hole in the heap or a held chunk

static pool_t             s_pools[MAX_POOLS]; // what the IDF wanted to add to the heap, held back until the quarantine is known
static size_t             s_npools;
static bool               s_released;
static psram_quar_piece_t s_holes[MAX_HOLES]; // the PSRAM left out of the heap: the quarantine (plus slivers too short for a heap)
static psram_quar_piece_t s_pool_pieces[MAX_POOLS]; // the pools as [start, end): the heap is these minus the holes (#89)
static size_t             s_nholes;
static size_t             s_hole_bytes, s_pieces, s_pieces_failed;
static chunk_t            s_chunks[MAX_CHUNKS]; // every chunk grabbed, sorted by address
static size_t             s_n;
static chunk_t            s_held[MAX_HELD]; // kept until reboot: chunks with bad bytes found this boot (holes next boot)
static size_t             s_nheld;
static psram_quar_t       s_quar;
static char               s_map[200];
static region_t           s_regions[MAX_REGIONS];
static size_t             s_nregions;
static uint8_t           *s_good[2]; // control pages for the sweep, taken from the heap on first use
static struct {
    size_t   scanned, bad_chunks, pass_bad[PASSES], bad_bytes;
    uint32_t xor_or, up_or, down_or;
    int64_t  us;
    uintptr_t lowest, highest;
    size_t   gaps_in_window, rounds;
    size_t   missing; // quarantined bytes neither a hole nor held: must be 0
    uintptr_t find_lo, find_hi; // lowest / highest bad byte of this scan
} s_stat;

// ---- the heap: PSRAM minus the quarantine -------------------------------------------------------------------------

esp_err_t __real_heap_caps_add_region_with_caps(uint32_t const caps[], intptr_t start, intptr_t end);

// The IDF registers the PSRAM with the heap at system start, before app_main. Those calls are recorded and answered with
// ESP_OK here; psram_scan_release_heap() registers the memory itself, cut around the quarantine. Until then PSRAM
// allocations fail and default mallocs fall back to internal RAM, as on a board without PSRAM; nothing in the start-up
// needs PSRAM. Regions added later (nobody does today) pass straight through.
esp_err_t __wrap_heap_caps_add_region_with_caps(uint32_t const caps[], intptr_t start, intptr_t end) {
    if (s_released || s_npools == MAX_POOLS || !caps) {
        return __real_heap_caps_add_region_with_caps(caps, start, end);
    }
    pool_t *p = &s_pools[s_npools++];
    memcpy(p->caps, caps, sizeof(p->caps));
    p->start = start;
    p->end   = end;
    return ESP_OK;
}

// The quarantine = every page that ever failed (learned set, includes this boot's finds), +- margin, merged
static void rebuild_quar(void) {
    psram_learn_t lr = psram_learned_get();
    s_quar           = (psram_quar_t){0};
    uintptr_t page;
    for (uintptr_t from = 0; psram_learn_next_failed(&lr, from, &page); from = page + PSRAM_PAGE) {
        psram_quar_note(&s_quar, page, page + PSRAM_PAGE - 1, 1, s_stat.xor_or);
    }
}

static void note_hole(uintptr_t lo, uintptr_t hi) {
    if (hi > lo && s_nholes < MAX_HOLES) {
        s_holes[s_nholes++] = (psram_quar_piece_t){lo, hi};
        s_hole_bytes += hi - lo;
    }
}

void psram_scan_release_heap(void) {
    psram_learned_load();
    rebuild_quar();
    for (size_t i = 0; i < s_npools; i++) {
        pool_t const      *p = &s_pools[i];
        s_pool_pieces[i]     = (psram_quar_piece_t){(uintptr_t)p->start, (uintptr_t)p->end};
        psram_quar_piece_t pieces[MAX_PIECES];
        size_t    n  = psram_quar_cut(&s_quar, (uintptr_t)p->start, (uintptr_t)p->end, PSRAM_QUAR_MIN_PIECE, pieces, MAX_PIECES);
        uintptr_t at = (uintptr_t)p->start;
        for (size_t k = 0; k < n; k++) {
            note_hole(at, pieces[k].lo);
            esp_err_t err = __real_heap_caps_add_region_with_caps(p->caps, (intptr_t)pieces[k].lo, (intptr_t)pieces[k].hi);
            if (err == ESP_OK) {
                s_pieces++;
            } else {
                s_pieces_failed++;
                ESP_LOGE(TAG, "heap: PSRAM piece %p..%p not registered: %s", (void *)pieces[k].lo, (void *)pieces[k].hi,
                         esp_err_to_name(err));
            }
            at = pieces[k].hi;
        }
        note_hole(at, (uintptr_t)p->end);
    }
    s_released = true;
    psram_learn_t lr = psram_learned_get();
    size_t        qb = psram_quar_bytes(&s_quar);
    ESP_LOGW(TAG, "heap: PSRAM registered in %u piece(s) (%u failed) from %u pool(s); %u hole(s) left out = %u KB: %u learned bad "
                  "pages in %u range(s) = %u KB quarantine, %u KB slivers too short for a heap",
             (unsigned)s_pieces, (unsigned)s_pieces_failed, (unsigned)s_npools, (unsigned)s_nholes, (unsigned)(s_hole_bytes >> 10),
             psram_learn_failed_pages(&lr), (unsigned)s_quar.n, (unsigned)(qb >> 10), (unsigned)((s_hole_bytes > qb ? s_hole_bytes - qb : 0) >> 10));
}

// ---- the free() guard (#89) ----------------------------------------------------------------------------------------
// heap_caps_free() aborts on a pointer outside every heap without saying what it was. The LVGL allocator asks here
// first: a pointer that is neither internal RAM nor in the PSRAM heap is logged with what it looks like and leaked.

static unsigned   s_bad_frees;
static void const *s_last_bad_free;

static void describe_bad_free(void const *p, char *out, size_t cap) {
    uintptr_t a = (uintptr_t)p;
    for (size_t i = 0; i < s_nholes; i++) {
        if (a >= s_holes[i].lo && a < s_holes[i].hi) {
            snprintf(out, cap, "in PSRAM quarantine hole %p..%p", (void *)s_holes[i].lo, (void *)s_holes[i].hi);
            return;
        }
    }
    snprintf(out, cap, esp_ptr_external_ram(p) ? "in PSRAM outside the heap pools" : "not RAM");
}

bool psram_guard_free_ok(void const *p) {
    if (!p || !s_released || esp_ptr_internal(p) ||
        psram_quar_in_heap(s_pool_pieces, s_npools, s_holes, s_nholes, (uintptr_t)p)) {
        return true;
    }
    unsigned n = ++s_bad_frees;
    s_last_bad_free = p;
    if (n <= 8 || n % 256 == 0) {
        char what[64];
        describe_bad_free(p, what, sizeof(what));
        int bit = psram_quar_flip_bit(s_pool_pieces, s_npools, s_holes, s_nholes, (uintptr_t)p);
        if (bit >= 0) {
            ESP_LOGW(TAG, "free(%p) refused: %s, bit %d flipped would be heap address %p; leaked (%u so far)", p, what, bit,
                     (void *)((uintptr_t)p ^ ((uintptr_t)1 << bit)), n);
        } else {
            ESP_LOGW(TAG, "free(%p) refused: %s, no single-bit neighbour in the heap; leaked (%u so far)", p, what, n);
        }
    }
    return false;
}

void psram_scan_print_frees(void) {
    if (s_bad_frees) {
        console_reply("psram frees refused: %u (outside every heap, leaked), last %p", s_bad_frees, s_last_bad_free);
    }
}

// ---- the boot scan ------------------------------------------------------------------------------------------------

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

static bool is_hole(uintptr_t lo, uintptr_t hi) {
    for (size_t i = 0; i < s_nholes; i++) {
        if (lo >= s_holes[i].lo && hi <= s_holes[i].hi) {
            return true;
        }
    }
    return false;
}

// Gaps between neighbouring chunks inside the window are the quarantine holes or memory somebody else already owns
// (code, rodata, early heap)
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
            bool hole = is_hole(end, next);
            if (next > WIN_LO && end < WIN_HI && !hole) {
                s_stat.gaps_in_window++;
            }
            ESP_LOGW(TAG, "map: %s %p..%p (%u KB)%s", hole ? "quarantine hole" : "in use", (void *)end, (void *)next,
                     (unsigned)((next - end) >> 10), next > WIN_LO && end < WIN_HI ? " in the scan window" : "");
        }
    }
}

// Chunks with new bad bytes (the quarantine grew into them) stay allocated until reboot; next boot they are holes
static void keep_or_free(void) {
    for (size_t i = 0; i < s_n; i++) {
        uintptr_t a = (uintptr_t)s_chunks[i].mem;
        if (psram_quar_overlaps(&s_quar, a, CHUNK) && s_nheld < MAX_HELD) {
            s_held[s_nheld++] = s_chunks[i];
            s_chunks[i].mem   = NULL;
        }
    }
    for (size_t i = 0; i < s_n; i++) {
        heap_caps_free(s_chunks[i].mem); // free(NULL) is fine
    }
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
        rebuild_quar();
        last = esp_timer_get_time() - t0;
        s_stat.rounds++;
    }
}

static bool held_covers(uintptr_t lo, uintptr_t hi) {
    for (size_t i = 0; i < s_nheld; i++) {
        uintptr_t m = (uintptr_t)s_held[i].mem;
        if (lo >= m && hi <= m + s_held[i].len) {
            return true;
        }
    }
    return false;
}

#define HEADER_GAP 256 // bytes between two neighbouring held chunks are the heap's block headers, nobody else's

// Bytes of [lo, hi) that are neither a hole nor inside a held chunk, ignoring the heap headers between held chunks.
// A whole-page gap is somebody's memory (a heap control block, an early allocation): counted.
static size_t uncovered_in(uintptr_t lo, uintptr_t hi) {
    size_t    missing = 0;
    uintptr_t at      = lo;
    while (at < hi) {
        uintptr_t next_lo = hi, cover_hi = 0; // the owned block starting at or after at that begins first; its end
        for (size_t i = 0; i < s_nholes + s_nheld; i++) {
            uintptr_t blo = i < s_nholes ? s_holes[i].lo : (uintptr_t)s_held[i - s_nholes].mem;
            uintptr_t bhi = i < s_nholes ? s_holes[i].hi : blo + s_held[i - s_nholes].len;
            if (bhi <= at || blo >= hi) {
                continue;
            }
            if (blo <= at) { // covers at: extend
                cover_hi = bhi > cover_hi ? bhi : cover_hi;
            } else if (blo < next_lo) {
                next_lo = blo;
            }
        }
        if (cover_hi > at) {
            at = cover_hi;
            continue;
        }
        size_t gap = (next_lo < hi ? next_lo : hi) - at;
        if (gap >= HEADER_GAP || next_lo >= hi) {
            missing += gap;
        }
        at = next_lo < hi ? next_lo : hi;
    }
    return missing;
}

static void coverage(void) {
    s_stat.missing = 0;
    for (size_t i = 0; i < s_quar.n; i++) {
        s_stat.missing += uncovered_in(s_quar.r[i].lo, s_quar.r[i].hi);
    }
}

void psram_scan_run(void) {
    int64_t start = esp_timer_get_time();
    size_t  free0 = heap_caps_get_free_size(PSRAM_CAPS);
    if (!s_released) {
        psram_scan_release_heap(); // app_main does this first; here only as a safety net
    }
    grab_all();
    log_map();
    drop_far();
    for (size_t i = 0; i < s_n; i++) {
        if (in_window(&s_chunks[i])) {
            scan_chunk(&s_chunks[i]);
        }
    }
    if (s_stat.find_hi) {
        psram_learned_feed(s_stat.find_lo, s_stat.find_hi, "boot scan");
    }
    rebuild_quar();
    extra_rounds(start);
    if (s_stat.find_hi) {
        psram_learned_feed(s_stat.find_lo, s_stat.find_hi, "boot scan");
    }
    rebuild_quar();
    keep_or_free();
    coverage();
    s_stat.us = esp_timer_get_time() - start;
    ESP_LOGW(TAG, "scan: window %p..%p, %u chunk scans + %u extra rounds in %u ms, %u bad chunks, bad bytes sparse %u dense %u/%u/%u; "
                  "flipped bits %08x (0->1 %08x, 1->0 %08x); quarantine %u range(s) %u KB: %u heap holes %u KB, %u chunks of 64 KB "
                  "held until reboot (new this boot), %u bytes NOT covered; free PSRAM %u -> %u KB",
             (void *)WIN_LO, (void *)WIN_HI, (unsigned)s_stat.scanned, (unsigned)s_stat.rounds, (unsigned)(s_stat.us / 1000),
             (unsigned)s_stat.bad_chunks, (unsigned)s_stat.pass_bad[0], (unsigned)s_stat.pass_bad[1], (unsigned)s_stat.pass_bad[2],
             (unsigned)s_stat.pass_bad[3], (unsigned)s_stat.xor_or, (unsigned)s_stat.up_or, (unsigned)s_stat.down_or,
             (unsigned)s_quar.n, (unsigned)(psram_quar_bytes(&s_quar) >> 10), (unsigned)s_nholes, (unsigned)(s_hole_bytes >> 10),
             (unsigned)s_nheld, (unsigned)s_stat.missing, (unsigned)(free0 >> 10),
             (unsigned)(heap_caps_get_free_size(PSRAM_CAPS) >> 10));
}

void psram_scan_note_region(char const *name, void const *addr, size_t len) {
    if (s_nregions < MAX_REGIONS) {
        s_regions[s_nregions++] = (region_t){name, (uintptr_t)addr, len};
    }
}

static char const *range_status(psram_quar_range_t const *r, char *buf, size_t cap) {
    bool hole = true;
    for (uintptr_t p = r->lo; p < r->hi; p += PSRAM_PAGE) {
        hole = hole && is_hole(p, p + PSRAM_PAGE);
    }
    size_t missing = uncovered_in(r->lo, r->hi);
    if (hole) {
        return "heap hole";
    }
    if (!missing) {
        return "held until reboot (new this boot)";
    }
    snprintf(buf, cap, "%u bytes NOT HELD (new this boot, hole from the next boot on)", (unsigned)missing);
    return buf;
}

void psram_scan_print(void) {
    console_reply("psram scan: window %p..%p, %u chunk scans + %u extra rounds in %u ms, %u bad chunks, bad bytes sparse %u dense %u/%u/%u, "
                  "bits %08x (0->1 %08x, 1->0 %08x), %u gaps (memory in use) in the window",
                  (void *)WIN_LO, (void *)WIN_HI, (unsigned)s_stat.scanned, (unsigned)s_stat.rounds, (unsigned)(s_stat.us / 1000),
                  (unsigned)s_stat.bad_chunks, (unsigned)s_stat.pass_bad[0], (unsigned)s_stat.pass_bad[1],
                  (unsigned)s_stat.pass_bad[2], (unsigned)s_stat.pass_bad[3], (unsigned)s_stat.xor_or,
                  (unsigned)s_stat.up_or, (unsigned)s_stat.down_or, (unsigned)s_stat.gaps_in_window);
    size_t held = 0;
    for (size_t i = 0; i < s_nheld; i++) {
        held += s_held[i].len;
    }
    console_reply("psram quarantine: %u ranges, %u KB; %u heap holes %u KB (left out of the heap at boot), %u chunks %u KB held until "
                  "reboot (new finds), %u bytes NOT covered; PSRAM heap in %u pieces from %u pools, %u pieces failed",
                  (unsigned)s_quar.n, (unsigned)(psram_quar_bytes(&s_quar) >> 10), (unsigned)s_nholes, (unsigned)(s_hole_bytes >> 10),
                  (unsigned)s_nheld, (unsigned)(held >> 10), (unsigned)s_stat.missing, (unsigned)s_pieces,
                  (unsigned)s_npools, (unsigned)s_pieces_failed);
    for (size_t i = 0; i < s_nholes; i++) {
        console_reply("psram hole %u: %p..%p (%u KB)", (unsigned)i, (void *)s_holes[i].lo, (void *)s_holes[i].hi,
                      (unsigned)((s_holes[i].hi - s_holes[i].lo) >> 10));
    }
    for (size_t i = 0; i < s_nheld; i++) {
        console_reply("psram scan chunk %p held: bad bytes sparse %u dense %u/%u/%u", (void *)s_held[i].mem,
                      (unsigned)s_held[i].bad[0], (unsigned)s_held[i].bad[1], (unsigned)s_held[i].bad[2],
                      (unsigned)s_held[i].bad[3]);
    }
    if (!s_quar.n) {
        console_reply("psram quarantine: none (no bad bytes in the window)");
    }
    for (size_t i = 0; i < s_quar.n; i++) {
        psram_quar_range_t const *r = &s_quar.r[i];
        char                      st[80];
        console_reply("psram quarantine %u: %p..%p (%u KB), failed pages %p..%p (%u), bits %08x: %s", (unsigned)i,
                      (void *)r->lo, (void *)r->hi, (unsigned)((r->hi - r->lo) >> 10), (void *)r->first_bad,
                      (void *)r->last_bad, (unsigned)r->bad_bytes, (unsigned)r->bits, range_status(r, st, sizeof(st)));
    }
    psram_learned_print();
    console_reply("psram map: %s", s_map);
    for (size_t i = 0; i < s_nregions; i++) {
        region_t const *g = &s_regions[i];
        console_reply("psram region %s %p..%p%s", g->name, (void *)g->addr, (void *)(g->addr + g->len),
                      psram_quar_overlaps(&s_quar, g->addr, g->len) ? " OVERLAPS the quarantine" : "");
    }
}

// ---- tests over the quarantine (holes + held chunks: ours alone) --------------------------------------------------

static size_t own_n(void) {
    return s_nholes + s_nheld;
}

static own_t own_at(size_t i) {
    if (i < s_nholes) {
        return (own_t){(uint8_t *)s_holes[i].lo, s_holes[i].hi - s_holes[i].lo};
    }
    return (own_t){s_held[i - s_nholes].mem, s_held[i - s_nholes].len};
}

static size_t own_bytes(void) {
    size_t sum = 0;
    for (size_t i = 0; i < own_n(); i++) {
        sum += own_at(i).len;
    }
    return sum;
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
    for (size_t i = 0; i < own_n(); i++) {
        own_t o = own_at(i);
        sync(o.mem, o.len);
        for (size_t off = 0; off < o.len; off += LINE) {
            psram_check_t r = psram_pattern_check_x(o.mem + off, LINE, (uintptr_t)o.mem + off, 0);
            if (!r.bad_bytes) {
                continue;
            }
            if (w->n < WATCH_MAX) {
                w->at[w->n] = (uintptr_t)o.mem + off + r.first_bad;
            }
            uintptr_t blo = (uintptr_t)o.mem + off + r.first_bad, bhi = (uintptr_t)o.mem + off + r.last_bad;
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

static void watch_line(char const *when, watch_t const *w) {
    console_reply("psram qwatch %s: %u bad lines, %u bad bytes, 0->1 %08x, 1->0 %08x", when, (unsigned)w->n, (unsigned)w->bytes,
                  (unsigned)w->up, (unsigned)w->down);
}

void psram_scan_watch(unsigned ms) {
    if (!own_n()) {
        console_reply("psram qwatch: no quarantine");
        return;
    }
    static watch_t a, b, c;
    for (size_t i = 0; i < own_n(); i++) {
        own_t o = own_at(i);
        psram_pattern_fill_x(o.mem, o.len, (uintptr_t)o.mem, 0);
        sync(o.mem, o.len);
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
    console_reply("psram qwatch: %u blocks (%u KB), wait %u ms; same first-bad byte in write/wait %u, wait/again %u of %u",
                  (unsigned)own_n(), (unsigned)(own_bytes() >> 10), ms, (unsigned)common(&a, &b), (unsigned)common(&b, &c),
                  (unsigned)b.n);
}

// "fw psram stress <rounds>": the boot scan's dense passes over the quarantine, on demand: pattern and inverted
// pattern alternately, each written back and read from the RAM. Bad bytes per round and in total; feeds the learned set.
void psram_scan_stress(unsigned rounds) {
    if (!own_n()) {
        console_reply("psram stress: no quarantine");
        return;
    }
    rounds = rounds ? (rounds > 64 ? 64 : rounds) : 8;
    size_t    total = 0, per[64] = {0};
    uint32_t  up = 0, down = 0;
    uintptr_t lo = 0, hi = 0;
    int64_t   t0 = esp_timer_get_time();
    for (unsigned r = 0; r < rounds; r++) {
        uint32_t mask = r & 1 ? 0xFFFFFFFFu : 0;
        for (size_t i = 0; i < own_n(); i++) {
            own_t o = own_at(i);
            psram_pattern_fill_x(o.mem, o.len, (uintptr_t)o.mem, mask);
            sync(o.mem, o.len);
        }
        for (size_t i = 0; i < own_n(); i++) {
            own_t         o = own_at(i);
            psram_check_t c = psram_pattern_check_x(o.mem, o.len, (uintptr_t)o.mem, mask);
            if (!c.bad_bytes) {
                continue;
            }
            per[r] += c.bad_bytes;
            up |= c.up_or;
            down |= c.down_or;
            uintptr_t blo = (uintptr_t)o.mem + c.first_bad, bhi = (uintptr_t)o.mem + c.last_bad;
            lo = !hi || blo < lo ? blo : lo;
            hi = bhi > hi ? bhi : hi;
            psram_learned_mark_buf(o.mem, o.len, (uintptr_t)o.mem, mask, c.first_bad, c.last_bad);
        }
        total += per[r];
        vTaskDelay(1);
    }
    if (hi) {
        psram_learned_feed(lo, hi, "stress");
    }
    char line[220];
    int  n = 0;
    for (unsigned r = 0; r < rounds && r < 32 && n < (int)sizeof(line) - 8; r++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", (unsigned)per[r]);
    }
    console_reply("psram stress: %u rounds (pattern, inverted, ...) over %u KB quarantine: %u bad bytes, 0->1 %08x, 1->0 %08x, %u ms; per round:%s",
                  rounds, (unsigned)(own_bytes() >> 10), (unsigned)total, (unsigned)up, (unsigned)down,
                  (unsigned)((esp_timer_get_time() - t0) / 1000), line);
}

// Pages for the delay-line sweep: up to max_bad quarantined 4 KB pages that failed before, spread over the set, and up
// to max_good pages from the heap outside the quarantine (never seen failing), kept for the next sweep
size_t psram_scan_test_pages(uint8_t **bad, size_t max_bad, uint8_t **good, size_t max_good, size_t *n_good) {
    psram_learn_t lr = psram_learned_get();
    uint8_t      *cand[512];
    size_t        nc = 0;
    uintptr_t     page;
    for (uintptr_t from = 0; nc < 512 && psram_learn_next_failed(&lr, from, &page); from = page + PSRAM_PAGE) {
        if (is_hole(page, page + PSRAM_PAGE) || held_covers(page, page + PSRAM_PAGE)) {
            cand[nc++] = (uint8_t *)page;
        }
    }
    size_t nb = nc < max_bad ? nc : max_bad;
    for (size_t k = 0; k < nb; k++) {
        bad[k] = cand[k * nc / nb];
    }
    *n_good = 0;
    for (size_t g = 0; g < max_good && g < 2; g++) {
        if (!s_good[g]) {
            s_good[g] = heap_caps_aligned_alloc(PSRAM_PAGE, PSRAM_PAGE, PSRAM_CAPS);
        }
        if (s_good[g]) {
            good[(*n_good)++] = s_good[g];
        }
    }
    return nb;
}
