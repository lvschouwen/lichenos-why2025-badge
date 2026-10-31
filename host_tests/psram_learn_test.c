// Host test: learned bad range of the PSRAM quarantine (#86)
#include "psram_learn.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    psram_learn_t r;
    psram_learn_reset(&r, 200);
    CHECK(psram_learn_empty(&r) && r.writes == 0);

    // First find: the range and the pages of both ends are learned (one write)
    CHECK(psram_learn_note(&r, 0x48558C01u, 0x4862E401u) == PSRAM_LEARN_GREW);
    CHECK(!psram_learn_empty(&r) && r.lo == 0x48558C01u && r.hi == 0x4862E401u && r.writes == 1);
    CHECK(psram_learn_failed_pages(&r) == 2);
    // Same bytes again: nothing to store
    CHECK(psram_learn_note(&r, 0x48558C01u, 0x48558C01u) == PSRAM_LEARN_SAME && r.writes == 1);
    CHECK(psram_learn_note(&r, 0x4862E400u, 0x4862E401u) == PSRAM_LEARN_SAME && r.writes == 1);
    // A new page between the ends changes the set (one write) though lo/hi stay
    CHECK(psram_learn_note(&r, 0x48560000u, 0x48560000u) == PSRAM_LEARN_GREW && r.writes == 2 && r.lo == 0x48558C01u);
    CHECK(psram_learn_note(&r, 0x48560010u, 0x48560010u) == PSRAM_LEARN_SAME && r.writes == 2);
    CHECK(psram_learn_note(&r, 0x48653C01u, 0x48653C01u) == PSRAM_LEARN_GREW && r.hi == 0x48653C01u && r.writes == 3);
    CHECK(psram_learn_note(&r, 0x48550000u, 0x48550003u) == PSRAM_LEARN_GREW && r.lo == 0x48550000u && r.writes == 4);
    CHECK(psram_learn_note(&r, 0x48653C01u, 0x48653C01u) == PSRAM_LEARN_SAME && r.writes == 4);

    // Outliers: outside PSRAM or reversed: ignored, nothing changes
    CHECK(psram_learn_note(&r, 0x3FC00000u, 0x3FC00010u) == PSRAM_LEARN_OUTLIER);
    CHECK(psram_learn_note(&r, 0x48600010u, 0x48600000u) == PSRAM_LEARN_OUTLIER);
    CHECK(psram_learn_note(&r, 0x4A000000u, 0x4A000000u) == PSRAM_LEARN_OUTLIER);
    CHECK(r.lo == 0x48550000u && r.hi == 0x48653C01u && r.writes == 4);
    // A far find is learned too (no span cap): it is its own page, not a stretch of the range
    unsigned before = psram_learn_failed_pages(&r);
    CHECK(psram_learn_note(&r, 0x487FBC01u, 0x487FBC01u) == PSRAM_LEARN_GREW && r.hi == 0x487FBC01u);
    CHECK(psram_learn_failed_pages(&r) == before + 1);

    // The failed set is what counts: iterate it, pages of both ends included, nothing in between
    uintptr_t pg;
    psram_learn_t p;
    psram_learn_reset(&p, 200);
    psram_learn_note(&p, 0x48558C01u, 0x487FBC01u);
    CHECK(psram_learn_failed_pages(&p) == 2);
    CHECK(psram_learn_next_failed(&p, 0, &pg) && pg == 0x48558000u);
    CHECK(psram_learn_next_failed(&p, pg + PSRAM_PAGE, &pg) && pg == 0x487FB000u);
    CHECK(!psram_learn_next_failed(&p, pg + PSRAM_PAGE, &pg));
    // Same bytes again: nothing new to store
    CHECK(psram_learn_note(&p, 0x48558C01u, 0x48558C05u) == PSRAM_LEARN_SAME);
    // A new page inside the extent is a change (stored), even if lo/hi stay
    CHECK(psram_learn_note(&p, 0x48600000u, 0x48600000u) == PSRAM_LEARN_GREW && p.lo == 0x48558C01u && p.hi == 0x487FBC01u);

    // Migration: an old record with only lo/hi (no map) is seeded with the pages of its ends; the span is not a failure
    psram_learn_t o;
    psram_learn_reset(&o, 200);
    o.lo = 0x48558C01u;
    o.hi = 0x487FBC01u;
    CHECK(psram_learn_failed_pages(&o) == 0);
    CHECK(psram_learn_seed(&o) && psram_learn_failed_pages(&o) == 2);
    CHECK(!psram_learn_seed(&o)); // idempotent: no rewrite on every boot
    psram_learn_t eo;
    psram_learn_reset(&eo, 200);
    CHECK(!psram_learn_seed(&eo) && psram_learn_failed_pages(&eo) == 0);

    // An empty record takes a first find anywhere in PSRAM
    psram_learn_t e;
    psram_learn_reset(&e, 200);
    CHECK(psram_learn_note(&e, 0x48400000u, 0x48400000u) == PSRAM_LEARN_GREW);

    // Persistence validity: size, version, speed, plausible range
    psram_learn_t s;
    psram_learn_reset(&s, 200);
    psram_learn_note(&s, 0x48558C01u, 0x4862E401u);
    CHECK(psram_learn_usable(&s, sizeof(s), 200));
    CHECK(!psram_learn_usable(&s, sizeof(s) - 1, 200));
    CHECK(!psram_learn_usable(&s, sizeof(s), 80)); // another PSRAM speed starts over
    psram_learn_t v = s;
    v.version++;
    CHECK(!psram_learn_usable(&v, sizeof(v), 200));
    psram_learn_t bad = s;
    bad.lo = 0x10000000u;
    CHECK(!psram_learn_usable(&bad, sizeof(bad), 200));
    psram_learn_t empty;
    psram_learn_reset(&empty, 200);
    CHECK(psram_learn_usable(&empty, sizeof(empty), 200));

    // Page failure map: first mark of a page reports "new", later ones do not; counted inside the span only
    psram_learn_t m;
    psram_learn_reset(&m, 200);
    m.lo = 0x48558C01u; // pages 0x48558000 .. 0x4855C000 = 5 pages
    m.hi = 0x4855CFFFu;
    CHECK(psram_learn_mark_page(&m, 0x48558C01u));
    CHECK(!psram_learn_mark_page(&m, 0x48558FFFu)); // same page
    CHECK(psram_learn_mark_page(&m, 0x4855AFFFu));
    CHECK(!psram_learn_mark_page(&m, 0x40000000u)); // outside the map window: not marked
    CHECK(psram_learn_mark_page(&m, 0x48600000u));  // inside the map but outside the span: kept, not counted
    unsigned span, failed;
    psram_learn_page_counts(&m, &span, &failed);
    CHECK(span == 5 && failed == 2);
    char map[16];
    CHECK(psram_learn_page_map(&m, map, sizeof(map)) == 5);
    CHECK(map[0] == '#' && map[1] == '.' && map[2] == '#' && map[3] == '.' && map[4] == '.' && map[5] == 0);
    psram_learn_t none;
    psram_learn_reset(&none, 200);
    psram_learn_page_counts(&none, &span, &failed);
    CHECK(span == 0 && failed == 0 && psram_learn_page_map(&none, map, sizeof(map)) == 0);
    CHECK(psram_learn_usable(&m, sizeof(m), 200));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_learn_test ok\n");
    return 0;
}
