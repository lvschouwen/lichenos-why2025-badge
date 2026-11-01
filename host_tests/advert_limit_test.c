// Host test: the new-node advert rate limit (#6 item 8)
#include "advert_limit.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int take_n(advert_limit_t *l, uint32_t now, int n) {
    int ok = 0;
    for (int i = 0; i < n; i++) {
        ok += advert_limit_take(l, now) == ADVERT_LIMIT_OK;
    }
    return ok;
}

int main(void) {
    advert_limit_t l;

    // A burst goes through whole, the next one is refused, and only the first refusal says so
    advert_limit_init(&l);
    CHECK(take_n(&l, 1000, ADVERT_LIMIT_BURST) == ADVERT_LIMIT_BURST);
    CHECK(advert_limit_take(&l, 1000) == ADVERT_LIMIT_REFUSED_FIRST);
    CHECK(advert_limit_take(&l, 1001) == ADVERT_LIMIT_REFUSED);
    CHECK(l.refused == 2);

    // One token per interval; a part of an interval is kept for the next one
    CHECK(advert_limit_take(&l, 1000 + ADVERT_LIMIT_EVERY_MS - 1) == ADVERT_LIMIT_REFUSED);
    CHECK(advert_limit_take(&l, 1000 + ADVERT_LIMIT_EVERY_MS) == ADVERT_LIMIT_OK);
    CHECK(advert_limit_take(&l, 1000 + ADVERT_LIMIT_EVERY_MS) == ADVERT_LIMIT_REFUSED_FIRST);
    CHECK(advert_limit_take(&l, 1000 + 2 * ADVERT_LIMIT_EVERY_MS + ADVERT_LIMIT_EVERY_MS / 2) == ADVERT_LIMIT_OK);
    CHECK(advert_limit_take(&l, 1000 + 3 * ADVERT_LIMIT_EVERY_MS) == ADVERT_LIMIT_OK);

    // A flood at one advert per 100 ms for 10 minutes gets about one in thirty through
    advert_limit_init(&l);
    int passed = 0;
    for (uint32_t t = 0; t < 600000; t += 100) {
        passed += advert_limit_take(&l, t) == ADVERT_LIMIT_OK;
    }
    CHECK(passed == ADVERT_LIMIT_BURST + 600000 / ADVERT_LIMIT_EVERY_MS - 1);

    // A long quiet spell tops up to the burst, never beyond
    advert_limit_init(&l);
    take_n(&l, 0, ADVERT_LIMIT_BURST);
    CHECK(take_n(&l, 3600000, 2 * ADVERT_LIMIT_BURST) == ADVERT_LIMIT_BURST);

    // Across the 32-bit millisecond wrap (49.7 days)
    advert_limit_init(&l);
    take_n(&l, 0xFFFFFFFFu - 100, ADVERT_LIMIT_BURST);
    CHECK(advert_limit_take(&l, 0xFFFFFFFFu - 100) == ADVERT_LIMIT_REFUSED_FIRST);
    CHECK(advert_limit_take(&l, ADVERT_LIMIT_EVERY_MS - 101) == ADVERT_LIMIT_OK);
    CHECK(advert_limit_take(&l, ADVERT_LIMIT_EVERY_MS - 101) == ADVERT_LIMIT_REFUSED_FIRST);

    if (failures) {
        printf("advert_limit_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("advert_limit_test: all passed\n");
    return 0;
}
