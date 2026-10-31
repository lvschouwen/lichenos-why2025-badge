// Host test: PSRAM quarantine range math (#86)
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

#define K 1024u

int main(void) {
    psram_quar_t q = {0};
    CHECK(psram_quar_bytes(&q) == 0 && !psram_quar_overlaps(&q, 0x48000000u, 0x10000000u));

    // One bad byte: margin each side, rounded outwards to 64 KB
    psram_quar_note(&q, 0x4855BA81u, 0x4855BA81u, 1, 0x100);
    CHECK(q.n == 1);
    CHECK(q.r[0].lo == 0x484D0000u); // 0x4855BA81 - 512K = 0x484DBA81 -> down 0x484D0000
    CHECK(q.r[0].lo % (64 * K) == 0 && q.r[0].hi % (64 * K) == 0);
    CHECK(q.r[0].hi >= 0x4855BA82u + PSRAM_QUAR_MARGIN && q.r[0].hi < 0x4855BA82u + PSRAM_QUAR_MARGIN + 64 * K);
    CHECK(q.r[0].first_bad == 0x4855BA81u && q.r[0].bits == 0x100);

    // A second find inside the range widens it, bits and counts accumulate
    psram_quar_note(&q, 0x48562000u, 0x48562003u, 4, 0x1);
    CHECK(q.n == 1 && q.r[0].last_bad == 0x48562003u && q.r[0].bits == 0x101 && q.r[0].bad_bytes == 5);
    CHECK(q.r[0].hi >= 0x48562004u + PSRAM_QUAR_MARGIN);

    // Overlap test is half-open
    CHECK(psram_quar_overlaps(&q, q.r[0].lo, 1));
    CHECK(!psram_quar_overlaps(&q, q.r[0].lo - 1, 1));
    CHECK(psram_quar_overlaps(&q, q.r[0].lo - 10, 11));
    CHECK(!psram_quar_overlaps(&q, q.r[0].hi, 100));
    CHECK(psram_quar_overlaps(&q, q.r[0].hi - 1, 100));

    // A far find stays a separate, sorted range; one below it sorts first
    psram_quar_note(&q, 0x48A00000u, 0x48A00000u, 1, 0x4);
    psram_quar_note(&q, 0x48100000u, 0x48100000u, 1, 0x8);
    CHECK(q.n == 3 && q.r[0].first_bad == 0x48100000u && q.r[2].first_bad == 0x48A00000u);
    CHECK(q.r[0].hi <= q.r[1].lo && q.r[1].hi <= q.r[2].lo);
    CHECK(psram_quar_bytes(&q) == (q.r[0].hi - q.r[0].lo) + (q.r[1].hi - q.r[1].lo) + (q.r[2].hi - q.r[2].lo));

    // Margins that touch merge (2 margins + 64 KB apart: ranges meet), 4 margins apart do not
    psram_quar_t m = {0};
    psram_quar_note(&m, 0x48200000u, 0x48200000u, 1, 1);
    psram_quar_note(&m, 0x48200000u + 2 * PSRAM_QUAR_MARGIN + 64 * K, 0x48200000u + 2 * PSRAM_QUAR_MARGIN + 64 * K, 1, 2);
    CHECK(m.n == 1);
    psram_quar_t f = {0};
    psram_quar_note(&f, 0x48200000u, 0x48200000u, 1, 1);
    psram_quar_note(&f, 0x48200000u + 4 * PSRAM_QUAR_MARGIN, 0x48200000u + 4 * PSRAM_QUAR_MARGIN, 1, 2);
    CHECK(f.n == 2);

    // More scattered finds than slots: the table stays at MAX, the nearest pair joins, nothing is dropped
    psram_quar_t s = {0};
    uintptr_t    at[] = {0x48100000u, 0x48500000u, 0x48900000u, 0x48D00000u, 0x49100000u, 0x49500000u};
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        psram_quar_note(&s, at[i], at[i], 1, 1);
        CHECK(s.n <= PSRAM_QUAR_MAX);
    }
    CHECK(s.n == PSRAM_QUAR_MAX); // six separate finds, four slots: the nearest pair were joined
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        CHECK(psram_quar_overlaps(&s, at[i], 1));
    }

    // Near the bottom of the address space: no wrap-around
    psram_quar_t z = {0};
    psram_quar_note(&z, 0x1000u, 0x1000u, 1, 1);
    CHECK(z.r[0].lo == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_quar_test ok\n");
    return 0;
}
