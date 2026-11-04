// Host test: only the radio's owner may send or change the radio (#179), and a refused caller can't flood the log
#include "lora_owner.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_owner(void) {
    lora_owner_t o = {0};
    CHECK(!lora_owner_is(&o, "meshcore")); // nobody holds it: no send yet
    CHECK(!lora_owner_is(&o, NULL));

    CHECK(lora_owner_claim(&o, "meshcore"));
    CHECK(lora_owner_is(&o, "meshcore"));  // the owner's send passes
    CHECK(!lora_owner_is(&o, "other"));    // a non-owner's send is refused
    CHECK(!lora_owner_is(&o, NULL));
    CHECK(!lora_owner_is(&o, "meshcor"));  // the whole name, not a prefix

    char copy[] = "meshcore"; // a different pointer with the same name is the same owner
    CHECK(lora_owner_is(&o, copy));
    CHECK(lora_owner_claim(&o, "meshcore")); // claiming again (MeshCore restarted) is fine

    CHECK(!lora_owner_claim(&o, "other")); // a second claim is refused, the first owner keeps it
    CHECK(lora_owner_is(&o, "meshcore"));
    CHECK(!lora_owner_claim(&o, NULL));

    CHECK(!lora_owner_release(&o, "other")); // only the owner can release
    CHECK(lora_owner_is(&o, "meshcore"));

    CHECK(lora_owner_release(&o, "meshcore"));
    CHECK(!lora_owner_is(&o, "meshcore")); // refused after release
    CHECK(!lora_owner_release(&o, "meshcore"));

    CHECK(lora_owner_claim(&o, "other")); // free again: the next app may claim it
    CHECK(lora_owner_is(&o, "other"));
    CHECK(!lora_owner_is(&o, "meshcore"));
}

static void test_refusal_log(void) {
    lora_refusal_log_t r = {0};
    uint32_t           skipped = 99;
    CHECK(lora_refusal_due(&r, 0, &skipped)); // the first refusal is always logged, even at t = 0
    CHECK(skipped == 0);
    for (uint32_t t = 1; t < LORA_REFUSAL_LOG_MS; t += 100) {
        CHECK(!lora_refusal_due(&r, t, &skipped)); // a spamming caller: quiet for the rest of the window
    }
    CHECK(lora_refusal_due(&r, LORA_REFUSAL_LOG_MS, &skipped));
    CHECK(skipped == 100); // and the next line says how many were swallowed
    CHECK(lora_refusal_due(&r, 3 * LORA_REFUSAL_LOG_MS, &skipped));
    CHECK(skipped == 0);

    // The millisecond clock wraps after 49 days
    lora_refusal_log_t w = {.logged = true, .last_ms = UINT32_MAX - 10};
    CHECK(!lora_refusal_due(&w, 100, &skipped));
    CHECK(lora_refusal_due(&w, LORA_REFUSAL_LOG_MS, &skipped));
    CHECK(skipped == 1);
}

int main(void) {
    test_owner();
    test_refusal_log();
    if (failures) {
        printf("lora_owner_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("lora_owner_test: ok\n");
    return 0;
}
