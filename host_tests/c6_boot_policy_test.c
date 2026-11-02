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

static c6_plan_t plan_t(c6_reset_kind_t kind, bool force, bool verify, bool trial, char const *stored, char const *cur) {
    return c6_boot_plan(kind, force, verify, trial, stored, cur, NULL);
}

static c6_plan_t plan(c6_reset_kind_t kind, bool force, bool verify, char const *stored, char const *cur) {
    return plan_t(kind, force, verify, false, stored, cur);
}

// Exactly one row of kind x force x verify x trial x keys gives SKIP
static void test_plan_table(void) {
    char const *keys[] = {NULL, "", "k1", "k2"};
    int         skips  = 0;
    for (int k = 0; k < 2; k++) {
        for (int f = 0; f < 2; f++) {
            for (int v = 0; v < 2; v++) {
                for (int t = 0; t < 2; t++) {
                    for (int s = 0; s < 4; s++) {
                        for (int c = 0; c < 4; c++) {
                            c6_plan_t p = plan_t(k ? C6_RST_OTHER : C6_RST_SOFTWARE, f, v, t, keys[s], keys[c]);
                            if (p == C6_PLAN_SKIP) {
                                skips++;
                                CHECK(k == 0 && f == 0 && v == 0 && t == 0 && s >= 2 && s == c);
                            }
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
    CHECK(c6_boot_plan(C6_RST_SOFTWARE, false, false, false, "k", "k", &why) == C6_PLAN_SKIP && why && why[0]);
    CHECK(c6_boot_plan(C6_RST_OTHER, false, false, false, "k", "k", &why) == C6_PLAN_FULL_CHECK && why && why[0]);
    // a trial (PENDING_VERIFY) image never skips: a restart now would roll the update back
    CHECK(c6_boot_plan(C6_RST_SOFTWARE, false, false, true, "k", "k", &why) == C6_PLAN_FULL_CHECK && why && why[0]);
}

// What the glue writes after the attach and IDENT, and whether it may restart (findings 1 and 3 of the #36 review)
static void test_after_attach(void) {
    c6_state_write_t w;
    // full path, attached, IDENT matches, image fine: store the key, no verify flag
    w = c6_state_after_attach(true, true, true, true, true);
    CHECK(w.write && w.store_key && !w.verify);
    // full path, attached, image check failed or no readable key: forget the key
    w = c6_state_after_attach(true, true, true, false, true);
    CHECK(w.write && !w.store_key && !w.verify);
    w = c6_state_after_attach(true, true, true, true, false);
    CHECK(w.write && !w.store_key && !w.verify);
    // full path, attached, IDENT not proven (old C6 image, mismatch, no answer): forget the key so no boot skips, but no
    // verify flag and no restart: every boot simply checks fully, as before #36
    w = c6_state_after_attach(true, true, false, true, true);
    CHECK(w.write && !w.store_key && !w.verify);
    // full path, attach failed: never store the key, set verify
    w = c6_state_after_attach(true, false, true, true, true);
    CHECK(w.write && !w.store_key && w.verify);
    w = c6_state_after_attach(true, false, false, true, true);
    CHECK(w.write && !w.store_key && w.verify);
    // skip path, attached, IDENT matches: nothing to write
    w = c6_state_after_attach(false, true, true, true, true);
    CHECK(!w.write);
    // skip path, attached, IDENT not proven: erase the key, set verify
    w = c6_state_after_attach(false, true, false, true, true);
    CHECK(w.write && !w.store_key && w.verify);
    // skip path, attach failed: erase the key, set verify
    w = c6_state_after_attach(false, false, true, true, true);
    CHECK(w.write && !w.store_key && w.verify);

    // restart only after a failed skip (attach or IDENT) whose NVS write persisted
    CHECK(c6_restart_after_attach(false, false, true, true));
    CHECK(c6_restart_after_attach(false, true, false, true));    // IDENT not proven after a skip
    CHECK(!c6_restart_after_attach(false, false, true, false));  // NVS write failed: would loop
    CHECK(!c6_restart_after_attach(false, true, false, false));  // same for IDENT
    CHECK(!c6_restart_after_attach(true, false, true, true));    // full path never restarts
    CHECK(!c6_restart_after_attach(true, true, false, true));    // not even for IDENT
    CHECK(!c6_restart_after_attach(false, true, true, true));    // attached and proven
}

static void test_ident(void) {
    uint8_t a[C6_IDENT_SHA_LEN], b[C6_IDENT_SHA_LEN];
    memset(a, 0x11, sizeof(a));
    memcpy(b, a, sizeof(b));
    CHECK(c6_ident_check(a, b) == C6_IDENT_MATCH);
    b[31] ^= 1;
    CHECK(c6_ident_check(a, b) == C6_IDENT_MISMATCH);
    b[31] ^= 1;
    b[0] ^= 0x80;
    CHECK(c6_ident_check(a, b) == C6_IDENT_MISMATCH);
    CHECK(c6_ident_check(NULL, b) == C6_IDENT_NO_ANSWER);
    CHECK(c6_ident_check(a, NULL) == C6_IDENT_NO_EXPECTED);
    CHECK(c6_ident_check(NULL, NULL) == C6_IDENT_NO_ANSWER);
    // every outcome has a name for the log, and only MATCH proves anything
    for (int i = C6_IDENT_NOT_ASKED; i <= C6_IDENT_NO_EXPECTED; i++) {
        CHECK(c6_ident_name((c6_ident_t)i)[0] != '\0');
    }
    CHECK(c6_ident_name((c6_ident_t)99)[0] != '\0');
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

// SW restarts in a row, driven by the pure decisions only. Each boot ends in the fallback restart or, when the badge
// stays up, in the next "fw restart". Counts the fallback restarts and the boots that skipped.
typedef struct {
    int restarts;
    int skips;
} sim_t;

static sim_t simulate_boots(bool nvs_ok, bool trial, bool attaches, bool ident_ok, char const *start_key, int boots) {
    char  stored[8];
    bool  verify = false;
    sim_t r      = {0, 0};
    snprintf(stored, sizeof(stored), "%s", start_key);
    for (int boot = 0; boot < boots; boot++) {
        bool full = plan_t(C6_RST_SOFTWARE, false, verify, trial, stored, "k") == C6_PLAN_FULL_CHECK;
        r.skips  += !full;
        bool             ident     = attaches && ident_ok; // IDENT is only asked once attached
        c6_state_write_t w         = c6_state_after_attach(full, attaches, ident, true, true);
        bool             persisted = false;
        if (w.write && nvs_ok) {
            snprintf(stored, sizeof(stored), "%s", w.store_key ? "k" : "");
            verify    = w.verify;
            persisted = true;
        }
        r.restarts += c6_restart_after_attach(full, attaches, ident, persisted);
    }
    return r;
}

static void test_no_loop_glue(void) {
    sim_t r;
    // a C6 that never attaches: the skip fails once, every later boot is full and stays up
    r = simulate_boots(true, false, false, true, "k", 10);
    CHECK(r.restarts == 1 && r.skips == 1);
    r = simulate_boots(false, false, false, true, "k", 10); // NVS broken: never restarts
    CHECK(r.restarts == 0);
    r = simulate_boots(true, true, false, true, "k", 10); // trial image: full check at once, no restart
    CHECK(r.restarts == 0 && r.skips == 0);
    // a C6 that attaches but never proves its image (an image without IDENT, or the wrong one): one fallback, then full
    // boots that never store the key, so it never skips (or restarts) again
    r = simulate_boots(true, false, true, false, "k", 10);
    CHECK(r.restarts == 1 && r.skips == 1);
    r = simulate_boots(true, false, true, false, "", 10);
    CHECK(r.restarts == 0 && r.skips == 0);
    r = simulate_boots(false, false, true, false, "k", 10);
    CHECK(r.restarts == 0);
    // healthy: one full boot stores the key, every later SW boot skips
    r = simulate_boots(true, false, true, true, "", 10);
    CHECK(r.restarts == 0 && r.skips == 9);
}

int main(void) {
    test_key();
    test_plan_table();
    test_why();
    test_no_loop();
    test_after_attach();
    test_ident();
    test_no_loop_glue();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("c6_boot_policy_test: all passed\n");
    return 0;
}
