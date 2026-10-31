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

    // One bad byte: margin each side, rounded outwards to the 4 KB grain
    psram_quar_note(&q, 0x4855BA81u, 0x4855BA81u, 1, 0x100);
    CHECK(q.n == 1);
    CHECK(q.r[0].lo == 0x4855A000u); // one 4 KB page before the failed page 0x4855B000
    CHECK(q.r[0].hi == 0x4855D000u - 0u && psram_quar_bytes(&q) == 12 * 1024u); // failed page + one page each side
    CHECK(q.r[0].lo % PSRAM_QUAR_GRAIN == 0 && q.r[0].hi % PSRAM_QUAR_GRAIN == 0);
    CHECK(q.r[0].hi >= 0x4855BA82u + PSRAM_QUAR_MARGIN && q.r[0].hi < 0x4855BA82u + PSRAM_QUAR_MARGIN + PSRAM_QUAR_GRAIN);
    CHECK(q.r[0].first_bad == 0x4855BA81u && q.r[0].bits == 0x100);

    // A second find inside the range widens it, bits and counts accumulate
    psram_quar_note(&q, 0x4855E000u, 0x4855E003u, 4, 0x1);
    CHECK(q.n == 1 && q.r[0].last_bad == 0x4855E003u && q.r[0].bits == 0x101 && q.r[0].bad_bytes == 5);
    CHECK(q.r[0].hi >= 0x4855E004u + PSRAM_QUAR_MARGIN);

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

    // Margins that touch merge (2 margins + 1 grain apart: ranges meet), 4 margins apart do not
    psram_quar_t m = {0};
    psram_quar_note(&m, 0x48200000u, 0x48200000u, 1, 1);
    psram_quar_note(&m, 0x48200000u + 2 * PSRAM_QUAR_MARGIN + PSRAM_QUAR_GRAIN, 0x48200000u + 2 * PSRAM_QUAR_MARGIN + PSRAM_QUAR_GRAIN, 1, 2);
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
    CHECK(s.n == 6); // room for many separate ranges
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        CHECK(psram_quar_overlaps(&s, at[i], 1));
    }
    // More finds than slots: the table stays at MAX, nothing is dropped
    psram_quar_t t = {0};
    for (unsigned i = 0; i < PSRAM_QUAR_MAX + 5; i++) {
        psram_quar_note(&t, 0x48000000u + i * 0x400000u, 0x48000000u + i * 0x400000u, 1, 1);
        CHECK(t.n <= PSRAM_QUAR_MAX);
    }
    CHECK(t.n == PSRAM_QUAR_MAX);
    for (unsigned i = 0; i < PSRAM_QUAR_MAX + 5; i++) {
        CHECK(psram_quar_overlaps(&t, 0x48000000u + i * 0x400000u, 1));
    }

    // Near the bottom of the address space: no wrap-around
    psram_quar_t z = {0};
    psram_quar_note(&z, 0x1000u, 0x1000u, 1, 1);
    CHECK(z.r[0].lo == 0);

    // The learned extent of #86 as one range: one-page margins: about 1 MB for the known span
    psram_quar_t l = {0};
    psram_quar_note(&l, 0x48558C01u, 0x48653C01u, 0, 0);
    CHECK(l.n == 1 && l.r[0].lo == 0x48557000u && l.r[0].hi == 0x48655000u);
    CHECK(psram_quar_bytes(&l) == 1016 * K);

    // #86 shape: a dense cluster (pages 0x48558000..0x48612000, every 3rd page) plus one outlier at 0x487FBC01, fed
    // page by page in ascending order: one range for the cluster, one 12 KB range for the outlier
    psram_quar_t u = {0};
    for (uintptr_t a = 0x48558000u; a < 0x48612000u; a += 3 * PSRAM_QUAR_GRAIN) {
        psram_quar_note(&u, a, a + PSRAM_QUAR_GRAIN - 1, 1, 0x100);
    }
    psram_quar_note(&u, 0x487FBC01u, 0x487FBC01u, 1, 0x100);
    CHECK(u.n == 2);
    CHECK(u.r[0].lo == 0x48557000u && u.r[0].hi == 0x48611000u);
    CHECK(u.r[1].lo == 0x487FA000u && u.r[1].hi == 0x487FD000u); // an outlier costs 12 KB
    // The same cluster with a gap of 4 pages between failed pages stays separate ranges
    psram_quar_t g = {0};
    psram_quar_note(&g, 0x48558000u, 0x48558FFFu, 1, 1);
    psram_quar_note(&g, 0x48558000u + 4 * PSRAM_QUAR_GRAIN, 0x48558FFFu + 4 * PSRAM_QUAR_GRAIN, 1, 1);
    CHECK(g.n == 2 && psram_quar_bytes(&g) == 24 * K);
    CHECK(!psram_quar_overlaps(&u, 0x48700000u, 0x10000)); // between them: free again
    CHECK(psram_quar_overlaps(&u, 0x487FBC01u, 1));
    // Same set in descending order gives the same ranges
    psram_quar_t d = {0};
    psram_quar_note(&d, 0x487FBC01u, 0x487FBC01u, 1, 0x100);
    for (uintptr_t a = 0x4860F000u; a >= 0x48558000u && a < 0x48612000u; a -= 3 * PSRAM_QUAR_GRAIN) {
        psram_quar_note(&d, a, a + PSRAM_QUAR_GRAIN - 1, 1, 0x100);
    }
    CHECK(d.n == 2 && d.r[0].lo == u.r[0].lo && d.r[0].hi == u.r[0].hi && d.r[1].lo == u.r[1].lo && d.r[1].hi == u.r[1].hi);

    // Coverage check (#86): the claim is only good when every quarantined byte is inside a held span
    psram_quar_t c = {0};
    psram_quar_note(&c, 0x48557000u, 0x48557000u, 1, 1); // 0x48556000..0x48559000
    psram_span_t full[] = {{0x48550000u, 0x48560000u}};
    CHECK(psram_quar_uncovered(&c, full, 1, 32) == 0);
    psram_span_t two[] = {{0x48550000u, 0x48557000u}, {0x48557010u, 0x48560000u}}; // 16 B between blocks = header
    CHECK(psram_quar_uncovered(&c, two, 2, 32) == 0);
    psram_span_t hole[] = {{0x48550000u, 0x48557000u}, {0x48558000u, 0x48560000u}}; // 4 KB hole inside the range
    CHECK(psram_quar_uncovered(&c, hole, 2, 32) == 0x1000);
    psram_span_t low[] = {{0x48557000u, 0x48560000u}}; // first page of the range not held
    CHECK(psram_quar_uncovered(&c, low, 1, 32) == 0x1000);
    psram_span_t high[] = {{0x48550000u, 0x48558800u}}; // last half page not held
    CHECK(psram_quar_uncovered(&c, high, 1, 32) == 0x800);
    CHECK(psram_quar_uncovered(&c, NULL, 0, 32) == 0x3000);
    psram_quar_t none = {0};
    CHECK(psram_quar_uncovered(&none, NULL, 0, 32) == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_quar_test ok\n");
    return 0;
}
