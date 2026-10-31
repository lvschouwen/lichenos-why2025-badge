// Host test: cutting a heap pool around the PSRAM quarantine (#86)
#include "psram_quar.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define K    1024u
#define MIN  (32u * K)
#define POOL_LO 0x48400000u
#define POOL_HI 0x4A000000u

static size_t total(psram_quar_piece_t const *p, size_t n) {
    size_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += p[i].hi - p[i].lo;
    }
    return sum;
}

int main(void) {
    psram_quar_piece_t p[16];

    // Nothing quarantined: the pool is one piece
    psram_quar_t none = {0};
    CHECK(psram_quar_cut(&none, POOL_LO, POOL_HI, MIN, p, 16) == 1 && p[0].lo == POOL_LO && p[0].hi == POOL_HI);

    // One range in the middle: two pieces, the hole is exactly the range
    psram_quar_t one = {0};
    psram_quar_note(&one, 0x48558C01u, 0x48653C01u, 1, 0x100);
    CHECK(psram_quar_cut(&one, POOL_LO, POOL_HI, MIN, p, 16) == 2);
    CHECK(p[0].lo == POOL_LO && p[0].hi == one.r[0].lo);
    CHECK(p[1].lo == one.r[0].hi && p[1].hi == POOL_HI);
    CHECK(total(p, 2) == POOL_HI - POOL_LO - psram_quar_bytes(&one));

    // A range outside the pool changes nothing; one covering the pool's start or end just trims it
    CHECK(psram_quar_cut(&one, 0x49000000u, POOL_HI, MIN, p, 16) == 1 && p[0].lo == 0x49000000u && p[0].hi == POOL_HI);
    CHECK(psram_quar_cut(&one, 0x48560000u, POOL_HI, MIN, p, 16) == 1 && p[0].lo == one.r[0].hi && p[0].hi == POOL_HI);
    CHECK(psram_quar_cut(&one, POOL_LO, 0x48600000u, MIN, p, 16) == 1 && p[0].lo == POOL_LO && p[0].hi == one.r[0].lo);
    // A pool inside the range: nothing left
    CHECK(psram_quar_cut(&one, 0x48560000u, 0x48600000u, MIN, p, 16) == 0);

    // Pieces shorter than min are dropped (they join the quarantine): two ranges 8 KB apart make one hole, 48 KB apart two
    psram_quar_t near = {0};
    psram_quar_note(&near, 0x48600000u, 0x48600000u, 1, 1);
    psram_quar_note(&near, 0x48600000u + 12 * K + 8 * K, 0x48600000u + 12 * K + 8 * K, 1, 1);
    CHECK(near.n == 2 && near.r[1].lo - near.r[0].hi == 8 * K);
    CHECK(psram_quar_cut(&near, POOL_LO, POOL_HI, MIN, p, 16) == 2);
    CHECK(p[0].hi == near.r[0].lo && p[1].lo == near.r[1].hi);
    psram_quar_t far = {0};
    psram_quar_note(&far, 0x48600000u, 0x48600000u, 1, 1);
    psram_quar_note(&far, 0x48600000u + 12 * K + 48 * K, 0x48600000u + 12 * K + 48 * K, 1, 1);
    CHECK(psram_quar_cut(&far, POOL_LO, POOL_HI, MIN, p, 16) == 3);
    CHECK(p[1].lo == far.r[0].hi && p[1].hi == far.r[1].lo && p[1].hi - p[1].lo == 48 * K);
    // A short leftover at the pool's end is dropped too
    CHECK(psram_quar_cut(&far, POOL_LO, far.r[1].hi + 4 * K, MIN, p, 16) == 2);

    // The #86 shape: 11 ranges, 12 pieces; every piece is outside every range and in order
    psram_quar_t u = {0};
    uintptr_t    pages[] = {0x48558000u, 0x48603000u, 0x4863d000u, 0x48646000u, 0x48651000u, 0x4866e000u,
                            0x487d9000u, 0x487e3000u, 0x487eb000u, 0x487f3000u, 0x487fb000u};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        psram_quar_note(&u, pages[i], pages[i] + 4 * K - 1, 1, 0x100);
    }
    CHECK(u.n == 11);
    size_t n = psram_quar_cut(&u, POOL_LO, POOL_HI, 4 * K, p, 16);
    CHECK(n == 12);
    for (size_t i = 0; i < n; i++) {
        CHECK(!psram_quar_overlaps(&u, p[i].lo, p[i].hi - p[i].lo));
        CHECK(i == 0 || p[i].lo >= p[i - 1].hi);
    }
    CHECK(total(p, n) == POOL_HI - POOL_LO - psram_quar_bytes(&u));
    // Not enough room for all pieces: the first max are returned, the rest stay out of the heap
    CHECK(psram_quar_cut(&u, POOL_LO, POOL_HI, 4 * K, p, 3) == 3 && p[2].hi == u.r[2].lo);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_quar_cut_test ok\n");
    return 0;
}
