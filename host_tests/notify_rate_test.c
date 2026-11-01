// Host test: the per-source rate limit of notifications and the toast/feedback decision (#62)
#include "notify_rate.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_bucket(uint32_t start) {
    nrate_t t;
    memset(&t, 0, sizeof(t));
    // A burst of 5, then nothing until 10 s later
    for (int i = 0; i < NRATE_BURST; i++) {
        CHECK(nrate_take(&t, "mesh", start + (uint32_t)i));
    }
    CHECK(!nrate_take(&t, "mesh", start + 100));
    CHECK(!nrate_take(&t, "mesh", start + NRATE_REFILL_MS - 1));
    CHECK(nrate_take(&t, "mesh", start + NRATE_REFILL_MS));
    CHECK(!nrate_take(&t, "mesh", start + NRATE_REFILL_MS + 1));
    // Another source has its own bucket
    CHECK(nrate_take(&t, "lichen", start + NRATE_REFILL_MS + 2));
    // A long quiet spell refills to the burst, never more
    uint32_t later = start + 1000u * NRATE_REFILL_MS;
    for (int i = 0; i < NRATE_BURST; i++) {
        CHECK(nrate_take(&t, "mesh", later));
    }
    CHECK(!nrate_take(&t, "mesh", later));
}

static void test_slots(void) {
    nrate_t t;
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < NRATE_BURST; i++) {
        nrate_take(&t, "busy", 0);
    }
    CHECK(!nrate_take(&t, "busy", 1));
    // More sources than slots: the least recently seen one is forgotten (and so starts afresh)
    char name[8];
    for (int i = 0; i < NRATE_SLOTS; i++) {
        snprintf(name, sizeof(name), "s%d", i);
        CHECK(nrate_take(&t, name, 2));
    }
    CHECK(nrate_take(&t, "busy", 3)); // was evicted: a full bucket again
}

static void test_show(void) {
    nrate_show_t s = nrate_show(0, false, false, true);
    CHECK(s.toast && s.feedback);
    s = nrate_show(2, true, false, true); // silent
    CHECK(!s.toast && !s.feedback);
    s = nrate_show(2, false, false, false); // over the rate
    CHECK(!s.toast && !s.feedback);
    s = nrate_show(1, false, true, true); // do-not-disturb hides warnings
    CHECK(!s.toast && !s.feedback);
    s = nrate_show(2, false, true, true); // but errors toast, silently
    CHECK(s.toast && !s.feedback);
}

int main(void) {
    test_bucket(0);
    test_bucket(UINT32_MAX - 3000); // the uptime wraps mid-burst
    test_slots();
    test_show();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("notify_rate_test: ok\n");
    return 0;
}
