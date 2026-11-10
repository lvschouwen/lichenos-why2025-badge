// Host tests for the fixed Wi-Fi frame pools of the esp_hosted SDIO host (#260): accounting, empty pool, bad frees,
// link-down with frames still out, the stats line
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

#define DEPTH 4
static char  mem[DEPTH][16];
static void *blocks[DEPTH] = {mem[0], mem[1], mem[2], mem[3]};

static void fresh(eh_frame_pool_core_t *c, esp_hosted_frame_pool_stats_t *s) {
    memset(s, 0, sizeof(*s));
    CHECK(eh_frame_pool_core_init(c, blocks, DEPTH, 16));
}

// The block and RAM maths the Kconfig help and sdkconfig.defaults quote: V2 header 20 + payload 1524 -> 1600
static void sizes(void) {
    CHECK(eh_frame_pool_block_bytes(20 + 1524, 64) == 1600);
    CHECK(eh_frame_pool_block_bytes(1536, 64) == 1536);
    CHECK(eh_frame_pool_block_bytes(1, 64) == 64);
    CHECK(eh_frame_pool_block_bytes(100, 3) == 0); // not a power of two
    CHECK(eh_frame_pool_ram_bytes(10, 1600) + eh_frame_pool_ram_bytes(10, 1536) + 1536 == 32896);
}

static void init_rejects_bad_input(void) {
    eh_frame_pool_core_t c;
    void                *with_null[2] = {mem[0], NULL};
    CHECK(!eh_frame_pool_core_init(&c, blocks, 0, 16));
    CHECK(!eh_frame_pool_core_init(&c, blocks, EH_FRAME_POOL_MAX_DEPTH + 1, 16));
    CHECK(!eh_frame_pool_core_init(&c, with_null, 2, 16));
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
    // Still exactly DEPTH blocks to hand out
    int n = 0;
    while (eh_frame_pool_core_take(&c, &s)) {
        n++;
    }
    CHECK(n == DEPTH);
}

// Link-down with no frame out: the caller frees the blocks at once
static void close_empty(void) {
    eh_frame_pool_core_t          c;
    esp_hosted_frame_pool_stats_t s;
    fresh(&c, &s);
    CHECK(eh_frame_pool_core_close(&c));
    CHECK(!c.live);
    CHECK(eh_frame_pool_core_take(&c, &s) == NULL);
    CHECK(!eh_frame_pool_core_close(&c)); // twice frees nothing twice
}

// Link-down with frames still out (a sender between its get and the queue): no takes, and the last put says free now
static void close_with_frames_out(void) {
    eh_frame_pool_core_t          c;
    esp_hosted_frame_pool_stats_t s;
    fresh(&c, &s);
    void *a = eh_frame_pool_core_take(&c, &s);
    void *b = eh_frame_pool_core_take(&c, &s);
    CHECK(!eh_frame_pool_core_close(&c));
    CHECK(c.live);
    CHECK(eh_frame_pool_core_take(&c, &s) == NULL);
    CHECK(eh_frame_pool_core_put(&c, a, &s) == EH_FRAME_POOL_PUT_OK);
    CHECK(eh_frame_pool_core_put(&c, b, &s) == EH_FRAME_POOL_PUT_LAST);
    CHECK(!c.live && s.in_use == 0);
    // Freed: a stale free afterwards is not ours, never a write into freed blocks
    CHECK(eh_frame_pool_core_put(&c, b, &s) == EH_FRAME_POOL_PUT_NOT_OURS);
}

// The full 64-block mask
static void depth_64(void) {
    static char                   big[EH_FRAME_POOL_MAX_DEPTH];
    void                         *bl[EH_FRAME_POOL_MAX_DEPTH];
    eh_frame_pool_core_t          c;
    esp_hosted_frame_pool_stats_t s = {0};
    for (int i = 0; i < EH_FRAME_POOL_MAX_DEPTH; i++) {
        bl[i] = &big[i];
    }
    CHECK(eh_frame_pool_core_init(&c, bl, EH_FRAME_POOL_MAX_DEPTH, 1));
    int n = 0;
    while (eh_frame_pool_core_take(&c, &s)) {
        n++;
    }
    CHECK(n == EH_FRAME_POOL_MAX_DEPTH && s.peak == EH_FRAME_POOL_MAX_DEPTH);
}

static void fmt_line(void) {
    esp_hosted_frame_pool_stats_t s = {
        .depth = 10, .block = 1600, .in_use = 2, .peak = 7, .waits = 3, .drops = 1, .bad_frees = 0, .no_pool = 0};
    char   line[160];
    size_t n = esp_hosted_frame_pool_fmt(line, sizeof(line), "c6: pool tx", &s);
    CHECK(strcmp(line, "c6: pool tx depth 10 block 1600 ram 16000 in_use 2 peak 7 waits 3 drops 1 bad_frees 0 no_pool 0") ==
          0);
    CHECK(n == strlen(line));
    char small[8];
    esp_hosted_frame_pool_fmt(small, sizeof(small), "x", &s);
    CHECK(strlen(small) == 7);
}

int main(void) {
    sizes();
    init_rejects_bad_input();
    take_until_empty();
    bad_frees();
    close_empty();
    close_with_frames_out();
    depth_64();
    fmt_line();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("frame_pool_test: all passed\n");
    return 0;
}
