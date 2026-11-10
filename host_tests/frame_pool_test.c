// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0

// Host tests for the fixed Wi-Fi frame pools of the esp_hosted SDIO host (#260): accounting, empty pool, bad frees,
// link-down with frames still out, re-attach loops that free every block exactly once, stuck blocks, the stats line
#include "eh_frame_pool_core.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// A fake heap: every block it hands out is counted when the test frees it, so a double free or a leak shows
#define HEAP_BLOCKS 1024
#define BLOCK       16
static char mem[HEAP_BLOCKS][BLOCK];
static int  heap_used;
static int  freed[HEAP_BLOCKS];

static void heap_reset(void) {
    heap_used = 0;
    memset(freed, 0, sizeof(freed));
}

static void fake_reserve(void **blocks, int n) {
    for (int i = 0; i < n; i++) {
        blocks[i] = mem[heap_used++];
    }
}

static void fake_free(void const *p) {
    freed[((char const(*)[BLOCK])p) - mem]++;
}

static void fake_free_all(void **list, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        fake_free(list[i]);
    }
}

#define DEPTH 4
static void *blocks[DEPTH] = {mem[0], mem[1], mem[2], mem[3]};

static void fresh(eh_frame_pool_core_t *c, esp_hosted_frame_pool_stats_t *s) {
    memset(s, 0, sizeof(*s));
    CHECK(eh_frame_pool_core_init(c, blocks, DEPTH, BLOCK));
}

// The block and RAM maths the Kconfig help and sdkconfig.defaults quote: V2 header 20 + payload 1524 -> 1600
static void sizes(void) {
    CHECK(eh_frame_pool_block_bytes(20 + 1524, 64) == 1600);
    CHECK(eh_frame_pool_block_bytes(1536, 64) == 1536);
    CHECK(eh_frame_pool_block_bytes(1, 64) == 64);
    CHECK(eh_frame_pool_block_bytes(100, 3) == 0); // not a power of two
    CHECK(eh_frame_pool_ram_bytes(10, 1600) + eh_frame_pool_ram_bytes(10, 1536) == 31360);
}

static void init_rejects_bad_input(void) {
    eh_frame_pool_core_t c;
    void                *with_null[2] = {mem[0], NULL};
    CHECK(!eh_frame_pool_core_init(&c, blocks, 0, BLOCK));
    CHECK(!eh_frame_pool_core_init(&c, blocks, EH_FRAME_POOL_MAX_DEPTH + 1, BLOCK));
    CHECK(!eh_frame_pool_core_init(&c, with_null, 2, BLOCK));
    CHECK(!eh_frame_pool_core_init(&c, blocks, DEPTH, 0));
}

// The pool hands out each block once, then NULL (never more than depth), and counts the high-water mark
static void take_until_empty(void) {
    eh_frame_pool_core_t          c;
    esp_hosted_frame_pool_stats_t s;
    fresh(&c, &s);
    void *got[DEPTH];
    for (int i = 0; i < DEPTH; i++) {
        got[i] = eh_frame_pool_core_take(&c, &s);
        CHECK(got[i] != NULL);
        for (int j = 0; j < i; j++) {
            CHECK(got[j] != got[i]);
        }
    }
    CHECK(eh_frame_pool_core_take(&c, &s) == NULL);
    CHECK(s.in_use == DEPTH && s.peak == DEPTH);
    CHECK(eh_frame_pool_core_put(&c, got[2], &s) == EH_FRAME_POOL_PUT_OK);
    CHECK(s.in_use == DEPTH - 1 && s.peak == DEPTH);
    CHECK(eh_frame_pool_core_take(&c, &s) == got[2]);
}

// A double free or a foreign pointer never enters the free set
static void bad_frees(void) {
    eh_frame_pool_core_t          c;
    esp_hosted_frame_pool_stats_t s;
    fresh(&c, &s);
    void *p = eh_frame_pool_core_take(&c, &s);
    CHECK(eh_frame_pool_core_put(&c, p, &s) == EH_FRAME_POOL_PUT_OK);
    CHECK(eh_frame_pool_core_put(&c, p, &s) == EH_FRAME_POOL_PUT_DOUBLE);
    CHECK(s.bad_frees == 1 && s.in_use == 0);
    char other;
    CHECK(eh_frame_pool_core_put(&c, &other, &s) == EH_FRAME_POOL_PUT_NOT_OURS);
    CHECK(eh_frame_pool_core_put(&c, NULL, &s) == EH_FRAME_POOL_PUT_NOT_OURS);
    CHECK(!eh_frame_pool_core_owns(&c, &other));
    CHECK(eh_frame_pool_core_owns(&c, p));
    int n = 0;
    while (eh_frame_pool_core_take(&c, &s)) {
        n++;
    }
    CHECK(n == DEPTH);
}

// Closing hands over the free blocks at once; one still out is freed when it comes back, and only once
static void close_with_frames_out(void) {
    eh_frame_pool_core_t          c;
    esp_hosted_frame_pool_stats_t s;
    void                         *to_free[EH_FRAME_POOL_MAX_DEPTH];
    fresh(&c, &s);
    void *a = eh_frame_pool_core_take(&c, &s);
    CHECK(eh_frame_pool_core_close(&c, to_free) == DEPTH - 1);
    for (int i = 0; i < DEPTH - 1; i++) {
        CHECK(to_free[i] != a);
    }
    CHECK(c.live && eh_frame_pool_core_take(&c, &s) == NULL);
    CHECK(eh_frame_pool_core_put(&c, a, &s) == EH_FRAME_POOL_PUT_FREE);
    CHECK(!c.live && s.in_use == 0);
    CHECK(eh_frame_pool_core_put(&c, a, &s) == EH_FRAME_POOL_PUT_NOT_OURS); // freed: a stale put touches nothing
    CHECK(eh_frame_pool_core_close(&c, to_free) == 0);
}

static void depth_64(void) {
    void                         *bl[EH_FRAME_POOL_MAX_DEPTH];
    eh_frame_pool_core_t          c;
    esp_hosted_frame_pool_stats_t s = {0};
    heap_reset();
    fake_reserve(bl, EH_FRAME_POOL_MAX_DEPTH);
    CHECK(eh_frame_pool_core_init(&c, bl, EH_FRAME_POOL_MAX_DEPTH, BLOCK));
    int n = 0;
    while (eh_frame_pool_core_take(&c, &s)) {
        n++;
    }
    CHECK(n == EH_FRAME_POOL_MAX_DEPTH && s.peak == EH_FRAME_POOL_MAX_DEPTH);
}

static bool install(eh_frame_pool_dir_t *d) {
    void *bl[DEPTH];
    fake_reserve(bl, DEPTH);
    return eh_frame_pool_dir_install(d, bl, DEPTH, BLOCK, ESP_HOSTED_FRAME_POOL_MEM_PSRAM);
}

static void retire(eh_frame_pool_dir_t *d) {
    void *to_free[EH_FRAME_POOL_MAX_DEPTH] = {0};
    fake_free_all(to_free, eh_frame_pool_dir_retire(d, to_free));
}

static void put(eh_frame_pool_dir_t *d, void *p) {
    if (eh_frame_pool_dir_put(d, p)) {
        fake_free(p);
    }
}

// Direction basics: no pool = OFF, too big, a second link-up without link-down is refused (L5), stats follow
static void dir_basics(void) {
    eh_frame_pool_dir_t d = {0};
    eh_fp_get_t         why;
    heap_reset();
    CHECK(eh_frame_pool_dir_take(&d, 1, &why) == NULL && why == EH_FP_OFF);
    CHECK(install(&d));
    CHECK(d.st.depth == DEPTH && d.st.block == BLOCK && d.st.mem == ESP_HOSTED_FRAME_POOL_MEM_PSRAM);
    CHECK(!install(&d)); // refused, and its blocks stay the caller's
    CHECK(eh_frame_pool_dir_take(&d, BLOCK + 1, &why) == NULL && why == EH_FP_TOO_BIG);
    void *p = eh_frame_pool_dir_take(&d, BLOCK, &why);
    CHECK(p && why == EH_FP_GOT);
    put(&d, p);
    retire(&d);
    CHECK(d.st.depth == 0 && d.st.mem == ESP_HOSTED_FRAME_POOL_MEM_NONE);
    CHECK(eh_frame_pool_dir_take(&d, 1, &why) == NULL && why == EH_FP_OFF);
    for (int i = 0; i < DEPTH; i++) {
        CHECK(freed[i] == 1);
    }
}

// Re-attach loop: each link-down leaves two frames out, which come back late, after the next link-up. Every block
// is freed exactly once, nothing is lost, and the stats end at zero in use.
static void reattach_loop_late_returns(void) {
    eh_frame_pool_dir_t d = {0};
    eh_fp_get_t         why;
    void               *out[2] = {NULL, NULL};
    heap_reset();
    for (int iter = 0; iter < 50; iter++) {
        CHECK(install(&d));
        for (int i = 0; i < 2; i++) {
            put(&d, out[i]); // the previous link's frames, back late
        }
        CHECK(d.st.in_use == 0);
        for (int i = 0; i < 2; i++) {
            out[i] = eh_frame_pool_dir_take(&d, BLOCK, &why);
            CHECK(out[i] != NULL);
        }
        retire(&d);
    }
    put(&d, out[0]); // the last ones come back while the link is down
    put(&d, out[1]);
    retire(&d);
    for (int i = 0; i < heap_used; i++) {
        CHECK(freed[i] == 1);
    }
    CHECK(heap_used == 50 * DEPTH);
    CHECK(d.st.lost == 0 && d.st.bad_frees == 0 && d.st.in_use == 0 && d.st.no_pool == 0);
}

// A frame that never comes back (sent to a queue that was destroyed): only that block is lost, one link session
// later, and never freed; the link-ups keep getting a pool; a stale put of it is a bad free, not a free.
static void reattach_loop_stuck_block(void) {
    eh_frame_pool_dir_t d = {0};
    eh_fp_get_t         why;
    void               *stuck[10];
    heap_reset();
    for (int iter = 0; iter < 10; iter++) {
        CHECK(install(&d));
        stuck[iter] = eh_frame_pool_dir_take(&d, BLOCK, &why);
        CHECK(stuck[iter] != NULL);
        retire(&d);
    }
    retire(&d); // one more link-down: the last stuck block had its session too
    CHECK(d.st.lost == 10);
    CHECK(d.st.in_use == 0 && d.st.no_pool == 0);
    int stuck_seen = 0;
    for (int i = 0; i < heap_used; i++) {
        bool is_stuck = false;
        for (int j = 0; j < 10; j++) {
            is_stuck = is_stuck || mem[i] == (char *)stuck[j];
        }
        stuck_seen += is_stuck;
        CHECK(freed[i] == (is_stuck ? 0 : 1));
    }
    CHECK(stuck_seen == 10);
    put(&d, stuck[0]);
    CHECK(d.st.bad_frees == 1);
    CHECK(freed[((char(*)[BLOCK])stuck[0]) - mem] == 0);
}

static void fmt_line(void) {
    esp_hosted_frame_pool_stats_t s = {.depth     = 10,
                                       .block     = 1600,
                                       .in_use    = 2,
                                       .peak      = 7,
                                       .waits     = 3,
                                       .drops     = 1,
                                       .bad_frees = 0,
                                       .lost      = 0,
                                       .no_pool   = 0,
                                       .mem       = ESP_HOSTED_FRAME_POOL_MEM_PSRAM};
    char   line[192];
    size_t n = esp_hosted_frame_pool_fmt(line, sizeof(line), "c6: pool tx", &s);
    CHECK(strcmp(line, "c6: pool tx depth 10 block 1600 ram 16000 mem psram in_use 2 peak 7 waits 3 drops 1 bad_frees 0 "
                       "lost 0 no_pool 0") == 0);
    CHECK(n == strlen(line));
    // Which memory the pool got, as the bench reads it; a pool that is off says none
    s.mem = ESP_HOSTED_FRAME_POOL_MEM_INTERNAL;
    esp_hosted_frame_pool_fmt(line, sizeof(line), "p", &s);
    CHECK(strstr(line, " mem internal ") != NULL);
    s.mem = ESP_HOSTED_FRAME_POOL_MEM_NONE;
    esp_hosted_frame_pool_fmt(line, sizeof(line), "p", &s);
    CHECK(strstr(line, " mem none ") != NULL);
    char small[8];
    esp_hosted_frame_pool_fmt(small, sizeof(small), "x", &s);
    CHECK(strlen(small) == 7);
}

int main(void) {
    sizes();
    init_rejects_bad_input();
    take_until_empty();
    bad_frees();
    close_with_frames_out();
    depth_64();
    dir_basics();
    reattach_loop_late_returns();
    reattach_loop_stuck_block();
    fmt_line();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("frame_pool_test: all passed\n");
    return 0;
}
