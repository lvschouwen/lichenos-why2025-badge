// Host tests for the rule that lets a warm restart skip the C6 flasher check (#36)
#include "c6_boot_policy.h"

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

#define M1 "0123456789abcdef0123456789abcdef"
#define M2 "fedcba9876543210fedcba9876543210"
#define M3 "00112233445566778899aabbccddeeff"

static void test_key(void) {
    char a[C6_BUNDLE_KEY_MAX], b[C6_BUNDLE_KEY_MAX];
    CHECK(c6_bundle_key(M1, M2, M3, 10, 20, 30, a, sizeof(a)));
    CHECK(strcmp(a, M1 "," M2 "," M3 ",10,20,30") == 0);
    // case does not matter
    CHECK(c6_bundle_key("0123456789ABCDEF0123456789ABCDEF", M2, M3, 10, 20, 30, b, sizeof(b)));
    CHECK(strcmp(a, b) == 0);
    // a trailing newline after the 32 digits (as in the .md5 file) is ignored
    CHECK(c6_bundle_key(M1 "\n", M2, M3, 10, 20, 30, b, sizeof(b)));
    CHECK(strcmp(a, b) == 0);
    // every md5 and every size changes the key
    CHECK(c6_bundle_key(M2, M2, M3, 10, 20, 30, b, sizeof(b)) && strcmp(a, b) != 0);
    CHECK(c6_bundle_key(M1, M1, M3, 10, 20, 30, b, sizeof(b)) && strcmp(a, b) != 0);
    CHECK(c6_bundle_key(M1, M2, M1, 10, 20, 30, b, sizeof(b)) && strcmp(a, b) != 0);
    CHECK(c6_bundle_key(M1, M2, M3, 11, 20, 30, b, sizeof(b)) && strcmp(a, b) != 0);
    CHECK(c6_bundle_key(M1, M2, M3, 10, 21, 30, b, sizeof(b)) && strcmp(a, b) != 0);
    CHECK(c6_bundle_key(M1, M2, M3, 10, 20, 31, b, sizeof(b)) && strcmp(a, b) != 0);
    // rejected
    CHECK(!c6_bundle_key("0123", M2, M3, 10, 20, 30, b, sizeof(b)));
    CHECK(!c6_bundle_key(M1, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", M3, 10, 20, 30, b, sizeof(b)));
    CHECK(!c6_bundle_key(M1, M2, NULL, 10, 20, 30, b, sizeof(b)));
    CHECK(!c6_bundle_key(M1, M2, M3, 10, 20, 30, b, 40)); // too small
    CHECK(!c6_bundle_key(M1, M2, M3, 10, 20, 30, NULL, 0));
}

static c6_plan_t plan(c6_reset_kind_t kind, bool force, bool verify, char const *stored, char const *cur) {
    return c6_boot_plan(kind, force, verify, stored, cur, NULL);
}

// Exactly one row of kind x force x verify x keys gives SKIP
static void test_plan_table(void) {
    char const *keys[] = {NULL, "", "k1", "k2"};
    int         skips  = 0;
    for (int k = 0; k < 2; k++) {
        for (int f = 0; f < 2; f++) {
            for (int v = 0; v < 2; v++) {
                for (int s = 0; s < 4; s++) {
                    for (int c = 0; c < 4; c++) {
                        c6_plan_t p = plan(k ? C6_RST_OTHER : C6_RST_SOFTWARE, f, v, keys[s], keys[c]);
                        if (p == C6_PLAN_SKIP) {
                            skips++;
                            CHECK(k == 0 && f == 0 && v == 0 && s >= 2 && s == c);
                        }
                    }
                }
            }
        }
    }
    CHECK(skips == 2); // k1==k1 and k2==k2
}

static void test_why(void) {
    char const *why = NULL;
    CHECK(c6_boot_plan(C6_RST_SOFTWARE, false, false, "k", "k", &why) == C6_PLAN_SKIP && why && why[0]);
    CHECK(c6_boot_plan(C6_RST_OTHER, false, false, "k", "k", &why) == C6_PLAN_FULL_CHECK && why && why[0]);
}

// After a failed attach on the skip path the verify flag is set and the key erased: the next boot is full, and a full
// boot never asks for a restart, so there is no loop. The sequence is modelled here as the glue does it.
static void test_no_loop(void) {
    char stored[8] = "k";
    bool verify    = false;
    int  restarts  = 0;
    for (int boot = 0; boot < 6; boot++) {
        c6_plan_t p        = plan(C6_RST_SOFTWARE, false, verify, stored, "k");
        bool      attached = false; // the C6 never attaches
        verify             = false;  // the full path clears the flag
        if (p == C6_PLAN_SKIP && !attached) {
            verify    = true;
            stored[0] = '\0';
            restarts++;
        } else if (p == C6_PLAN_FULL_CHECK) {
            CHECK(boot > 0); // the first boot skipped
        }
    }
    CHECK(restarts == 1);
}

int main(void) {
    test_key();
    test_plan_table();
    test_why();
    test_no_loop();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("c6_boot_policy_test: all passed\n");
    return 0;
}
