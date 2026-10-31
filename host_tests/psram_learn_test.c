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

    // Seed with the first scan, then only growth counts as a write
    CHECK(psram_learn_note(&r, 0x48558C01u, 0x4862E401u) == PSRAM_LEARN_GREW);
    CHECK(!psram_learn_empty(&r) && r.lo == 0x48558C01u && r.hi == 0x4862E401u && r.writes == 1);
    CHECK(psram_learn_note(&r, 0x48560000u, 0x48600000u) == PSRAM_LEARN_SAME && r.writes == 1);
    CHECK(psram_learn_note(&r, 0x48558C01u, 0x48558C01u) == PSRAM_LEARN_SAME && r.writes == 1);
    CHECK(psram_learn_note(&r, 0x48653C01u, 0x48653C01u) == PSRAM_LEARN_GREW && r.hi == 0x48653C01u && r.writes == 2);
    CHECK(psram_learn_note(&r, 0x48550000u, 0x48550003u) == PSRAM_LEARN_GREW && r.lo == 0x48550000u && r.writes == 3);
    CHECK(psram_learn_note(&r, 0x48653C01u, 0x48653C01u) == PSRAM_LEARN_SAME && r.writes == 3);

    // Outliers: outside PSRAM, reversed, or stretching the span past the cap: ignored, nothing changes
    CHECK(psram_learn_note(&r, 0x3FC00000u, 0x3FC00010u) == PSRAM_LEARN_OUTLIER);
    CHECK(psram_learn_note(&r, 0x48600010u, 0x48600000u) == PSRAM_LEARN_OUTLIER);
    CHECK(psram_learn_note(&r, 0x49000000u, 0x49000000u) == PSRAM_LEARN_OUTLIER);
    CHECK(r.lo == 0x48550000u && r.hi == 0x48653C01u && r.writes == 3);
    // Just inside the cap still grows
    CHECK(psram_learn_note(&r, 0x48550000u, 0x48550000u + PSRAM_LEARN_MAX_SPAN - 1) == PSRAM_LEARN_GREW);
    CHECK(psram_learn_note(&r, 0x48550000u, 0x48550000u + PSRAM_LEARN_MAX_SPAN) == PSRAM_LEARN_OUTLIER);

    // An empty record takes a big first find only when it is plausible
    psram_learn_t e;
    psram_learn_reset(&e, 200);
    CHECK(psram_learn_note(&e, 0x48400000u, 0x48A00000u) == PSRAM_LEARN_OUTLIER && psram_learn_empty(&e));

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
    bad.hi = 0x49F00000u; // span far over the cap
    CHECK(!psram_learn_usable(&bad, sizeof(bad), 200));
    bad = s;
    bad.lo = 0x10000000u;
    CHECK(!psram_learn_usable(&bad, sizeof(bad), 200));
    psram_learn_t empty;
    psram_learn_reset(&empty, 200);
    CHECK(psram_learn_usable(&empty, sizeof(empty), 200));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_learn_test ok\n");
    return 0;
}
