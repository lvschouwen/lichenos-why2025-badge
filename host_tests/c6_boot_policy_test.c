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

// Every input combination against the rule written out once more (#212): the first reason that applies, in this order,
// is the one given, and only the last ("unchanged") skips. Pins the plan and the exact reason the log shows.
static void test_plan_exhaustive(void) {
    // stored and current keys in separate buffers: equal contents, never the same pointer, so only strcmp passes
    static char       stored_buf[3][2] = {"", "k", "j"}, current_buf[3][2] = {"", "k", "j"};
    char const *const stored_keys[]    = {NULL, stored_buf[0], stored_buf[1], stored_buf[2]};
    char const *const current_keys[]   = {NULL, current_buf[0], current_buf[1], current_buf[2]};
    for (int i = 0; i < 2 * 2 * 2 * 2 * 4 * 4; i++) {
        c6_reset_kind_t kind    = i & 1 ? C6_RST_OTHER : C6_RST_SOFTWARE;
        bool            force   = i & 2, verify = i & 4, trial = i & 8;
        char const     *stored  = stored_keys[(i >> 4) & 3];
        char const     *current = current_keys[(i >> 6) & 3];
        char const     *want    = kind != C6_RST_SOFTWARE        ? "not a software reset"
                                  : force                         ? "forced reflash"
                                  : verify                        ? "verify flag (last attach failed)"
                                  : trial                         ? "trial image (a restart would roll it back)"
                                  : !current || !current[0]       ? "no readable bundle"
                                  : !stored || !stored[0]         ? "no verified bundle stored"
                                  : strcmp(stored, current) != 0 ? "bundle changed"
                                                                  : "bundle unchanged since the last verify";
        char const     *why     = NULL;
        c6_plan_t       got     = c6_boot_plan(kind, force, verify, trial, stored, current, &why);
        c6_plan_t       plan    = strcmp(want, "bundle unchanged since the last verify") == 0 ? C6_PLAN_SKIP
                                                                                                : C6_PLAN_FULL_CHECK;
        if (got != plan || !why || strcmp(why, want) != 0) {
            printf("FAIL c6_boot_plan case %d: plan %d why %s, want %d %s\n", i, (int)got, why ? why : "NULL",
                   (int)plan, want);
            failures++;
        }
        CHECK(c6_boot_plan(kind, force, verify, trial, stored, current, NULL) == plan); // why may be NULL
    }
}

// #583: IDF's SDIO init leaves a High Speed link at 40 MHz; raise it to the configured 50, never a default-speed link
// (20 MHz: the C6 refused High Speed), never lower, never on no slot
static void test_sdio_raise(void) {
    CHECK(c6_sdio_raise_khz(50000, 40000) == 50000);
    CHECK(c6_sdio_raise_khz(50000, 50000) == 0);
    CHECK(c6_sdio_raise_khz(50000, 20000) == 0);
    CHECK(c6_sdio_raise_khz(40000, 40000) == 0);
    CHECK(c6_sdio_raise_khz(20000, 40000) == 0);
    CHECK(c6_sdio_raise_khz(50000, 0) == 0);
    CHECK(c6_sdio_raise_khz(50000, -1) == 0);
}

// #420: the on-air list waits while the staged bundle could still change the C6; a boot whose C6 proved it, or with no
// bundle staged, does not (else the list could never be switched on); a failed flash (IDENT no match) still waits
static void test_bundle_pending(void) {
    CHECK(!c6_bundle_pending(false, true, true, true));   // the usual boot: proven
    CHECK(!c6_bundle_pending(false, true, false, false)); // nothing staged: the flasher leaves the C6 alone
    CHECK(c6_bundle_pending(false, true, true, false));   // staged, not running (a failed flash, an older C6)
    CHECK(c6_bundle_pending(true, true, true, true));     // pushed or deleted this boot
    CHECK(c6_bundle_pending(true, true, false, false));
    CHECK(c6_bundle_pending(false, false, true, true));   // the C6 step has not run yet
    CHECK(c6_bundle_pending(false, false, false, false));
}


// #595: the card bundle, every row
static void test_card_classify(void) {
    char const *k1 = M1 "," M2 "," M3 ",10,20,30";
    char const *k2 = M2 "," M2 "," M3 ",10,20,30";
    CHECK(c6_card_classify(0, "", k1, false) == C6_CARD_NONE);
    CHECK(c6_card_classify(0, "", "", true) == C6_CARD_NONE);         // a stale record means nothing without files
    for (unsigned f = 1; f < 6; f++) {
        CHECK(c6_card_classify(f, k2, k1, false) == C6_CARD_HALF);   // a half bundle is never used
    }
    CHECK(c6_card_classify(6, "", k1, false) == C6_CARD_HALF);       // all there, a .md5 malformed
    CHECK(c6_card_classify(6, NULL, k1, false) == C6_CARD_HALF);
    CHECK(c6_card_classify(6, k1, k1, false) == C6_CARD_SAME);       // copied after the last card install
    CHECK(c6_card_classify(6, k1, k1, true) == C6_CARD_SAME);        // same as /storage beats a stale record
    CHECK(c6_card_classify(6, k2, k1, true) == C6_CARD_FAILED);
    CHECK(c6_card_classify(6, k2, k1, false) == C6_CARD_NEW);
    CHECK(c6_card_classify(6, k2, "", false) == C6_CARD_NEW);        // no bundle on /storage at all
    CHECK(c6_card_classify(6, k2, NULL, false) == C6_CARD_NEW);
    for (int c = C6_CARD_NONE; c <= C6_CARD_NEW; c++) {
        CHECK(c6_card_use((c6_card_t)c, true) == (c == C6_CARD_NEW));
        CHECK(!c6_card_use((c6_card_t)c, false));                    // a bad md5 is never the source
        CHECK(c6_card_name((c6_card_t)c)[0] != '?');
    }
}

static void test_card_record(void) {
    uint8_t sha[C6_IDENT_SHA_LEN], other[C6_IDENT_SHA_LEN];
    for (int i = 0; i < C6_IDENT_SHA_LEN; i++) {
        sha[i]   = (uint8_t)(i * 7);
        other[i] = (uint8_t)(i * 7);
    }
    other[31] ^= 1;
    char a[C6_CARD_RECORD_MAX], b[C6_CARD_RECORD_MAX];
    char const *k1 = M1 "," M2 "," M3 ",10,20,30";
    CHECK(c6_card_record(k1, sha, a, sizeof(a)));
    CHECK(strncmp(a, k1, strlen(k1)) == 0 && a[strlen(k1)] == '|' && strlen(a) == strlen(k1) + 1 + 64);
    CHECK(strcmp(a + strlen(k1) + 1 + 60, "d2d9") == 0); // bytes 30 and 31 (210, 217), lowercase hex
    CHECK(c6_card_record(k1, other, b, sizeof(b)) && strcmp(a, b) != 0); // another P4 image: another record
    CHECK(!c6_card_record("", sha, b, sizeof(b)) && b[0] == '\0');
    CHECK(!c6_card_record(k1, NULL, b, sizeof(b)) && b[0] == '\0');
    CHECK(!c6_card_record(k1, sha, b, strlen(k1) + 1 + 64) && b[0] == '\0'); // no room for the NUL
    CHECK(c6_card_record(k1, sha, b, strlen(k1) + 1 + 64 + 1));
    CHECK(!c6_card_record(k1, sha, NULL, 10));
}

static void test_card_after(void) {
    for (int w = 0; w < 2; w++) {
        for (int i = 0; i < 2; i++) {
            CHECK(c6_card_after(false, w, i) == C6_CARD_KEEP); // /storage/c6 was the source: the card is not touched
        }
    }
    CHECK(c6_card_after(true, true, true) == C6_CARD_COPY);
    CHECK(c6_card_after(true, false, true) == C6_CARD_COPY);           // the C6 already ran it: still the rescue copy
    // The retry under a new P4 image (the record is per P4): the C6 already holds the card bundle, so the flasher
    // writes nothing, and IDENT failing again must still record it, or every boot would retry it for ever
    CHECK(c6_card_after(true, true, false) == C6_CARD_RECORD_FAILED);
    CHECK(c6_card_after(true, false, false) == C6_CARD_KEEP);          // the flasher never reached the C6
}

// The contract-change case (#595 option A): a card with a new C6 and a new P4. The old P4 records the card bundle
// failed (the new C6 cannot answer its IDENT); the new P4 then retries it once, since the record names the old P4, and
// records it under its own SHA only if IDENT fails again.
static bool failed_under(char const *stored, char const *card_key, uint8_t const *p4) {
    char rec[C6_CARD_RECORD_MAX];
    return c6_card_record(card_key, p4, rec, sizeof(rec)) && strcmp(stored, rec) == 0;
}

static void test_card_contract_change(void) {
    char const *storage = M1 "," M2 "," M3 ",10,20,30";
    char const *card    = M3 "," M2 "," M1 ",10,20,30";
    uint8_t     old_p4[C6_IDENT_SHA_LEN], new_p4[C6_IDENT_SHA_LEN];
    memset(old_p4, 0x11, sizeof(old_p4));
    memset(new_p4, 0x22, sizeof(new_p4));
    char stored[C6_CARD_RECORD_MAX] = "";

    // Boot 1, old P4: the card bundle is new, flashed, IDENT fails -> recorded under the old P4
    CHECK(c6_card_classify(6, card, storage, failed_under(stored, card, old_p4)) == C6_CARD_NEW);
    CHECK(c6_card_after(true, true, false) == C6_CARD_RECORD_FAILED);
    CHECK(c6_card_record(card, old_p4, stored, sizeof(stored)));
    // The old P4 again (say the P4 install failed): the bundle stays ignored
    CHECK(c6_card_classify(6, card, storage, failed_under(stored, card, old_p4)) == C6_CARD_FAILED);

    // Boot 2, the new P4 from the card: the record is not its own, so the card bundle is the source again
    c6_card_t c = c6_card_classify(6, card, storage, failed_under(stored, card, new_p4));
    CHECK(c == C6_CARD_NEW && c6_card_use(c, true));
    // ... the C6 already holds it (no write), and the new P4 gets IDENT: copied, the rescue copy is updated
    CHECK(c6_card_after(true, true, true) == C6_CARD_COPY);
    // ... or IDENT fails under the new P4 too: recorded under the new P4, so boot 3 goes back to /storage/c6
    CHECK(c6_card_after(true, true, false) == C6_CARD_RECORD_FAILED);
    CHECK(c6_card_record(card, new_p4, stored, sizeof(stored)));
    CHECK(c6_card_classify(6, card, storage, failed_under(stored, card, new_p4)) == C6_CARD_FAILED);
}

int main(void) {
    test_key();
    test_bundle_pending();
    test_sdio_raise();
    test_plan_table();
    test_plan_exhaustive();
    test_why();
    test_no_loop();
    test_after_attach();
    test_ident();
    test_no_loop_glue();
    test_card_classify();
    test_card_record();
    test_card_after();
    test_card_contract_change();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("c6_boot_policy_test: all passed\n");
    return 0;
}
