// Host test: the PSRAM 80/120 MHz switch state machine (#121; 120 by default, evidence-only fallback since #353)
#include "psram_speed_logic.h"

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

static psram_speed_rec_t saved;
static bool save_ok(psram_speed_rec_t const *r) {
    saved = *r;
    return true;
}
static bool save_fail(psram_speed_rec_t const *r) {
    (void)r;
    return false;
}

// esp_reset_reason_t numbers (pinned in psram_speed.c)
enum {
    RST_POWERON  = 1,
    RST_EXT      = 2,
    RST_SW       = 3,
    RST_PANIC    = 4,
    RST_INT_WDT  = 5,
    RST_TASK_WDT = 6,
    RST_WDT      = 7, // also every power-on and CH340 reset on this board (HP_SYS_HP_WDT_RESET); a crash only with a new dump
    RST_BROWNOUT = 9,
    RST_LOCKUP   = 15,
};

static psram_fail_ctx_t at(uint8_t reset) {
    return (psram_fail_ctx_t){.reset = reset, .uptime_s = 3, .fw = "fw-605935b1"};
}

static psram_fail_ctx_t crashed_wdt(void) { // a real watchdog: reason 7 with a core dump new since the last crash
    psram_fail_ctx_t c = at(RST_WDT);
    c.fresh_dump       = true;
    return c;
}

static void test_default(void);
static void test_crash_class(void);
static void test_carried_fall(void);
static void test_no_evidence(void);
static void test_evidence(void);
static void test_switch_back(void);
static void test_arms(void);
static void test_texts(void);
static void test_record(void);

int main(void) {
    test_default();
    test_crash_class();
    test_carried_fall();
    test_no_evidence();
    test_evidence();
    test_switch_back();
    test_arms();
    test_texts();
    test_record();

    // Strict command parse
    unsigned tg = 0;
    CHECK(psram_speed_parse("", &tg) == PSRAM_SPEED_CMD_STATUS);
    CHECK(psram_speed_parse("  ", &tg) == PSRAM_SPEED_CMD_STATUS);
    CHECK(psram_speed_parse(" 120", &tg) == PSRAM_SPEED_CMD_ASK && tg == 120);
    CHECK(psram_speed_parse(" 80 yes", &tg) == PSRAM_SPEED_CMD_DO && tg == 80);
    CHECK(psram_speed_parse(" 120 yes", &tg) == PSRAM_SPEED_CMD_DO && tg == 120);
    CHECK(psram_speed_parse(" 120abc", &tg) == PSRAM_SPEED_CMD_INVALID);
    CHECK(psram_speed_parse(" 120 yesterday", &tg) == PSRAM_SPEED_CMD_INVALID);
    CHECK(psram_speed_parse(" 120 yes now", &tg) == PSRAM_SPEED_CMD_INVALID);
    CHECK(psram_speed_parse(" 100", &tg) == PSRAM_SPEED_CMD_INVALID);
    CHECK(psram_speed_parse(" -80", &tg) == PSRAM_SPEED_CMD_INVALID);
    CHECK(psram_speed_parse(" 0120 yes", &tg) == PSRAM_SPEED_CMD_INVALID);
    CHECK(psram_speed_parse(" yes", &tg) == PSRAM_SPEED_CMD_INVALID);
    CHECK(psram_speed_parse(" 120                                  junk", &tg) == PSRAM_SPEED_CMD_INVALID);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_speed_test ok\n");
    return 0;
}

// #353: a badge with nothing saved wants 120 and arms it from its first boot at 80
static void test_default(void) {
    psram_speed_rec_t r;
    psram_speed_rec_decode(NULL, 0, &r); // no record in NVS
    CHECK(r.want == 120 && !r.failed && r.arms == 0 && r.last.kind == PSRAM_FAIL_NONE);
    psram_fail_ctx_t on = at(RST_WDT); // a cold boot on this board: reason 7, no new core dump
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_ARM_RESTART);
    CHECK(psram_speed_arm_commit(&r, save_ok) && saved.arms == 1 && saved.want == 120);
    CHECK(psram_speed_boot_decide(&r, 120, true, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_NONE && r.want == 120);
    // A record that is not ours is the default too
    r = (psram_speed_rec_t){.want = 100};
    psram_speed_rec_sane(&r);
    CHECK(r.want == 120 && !r.failed);
    // IDF not patched: nothing happens, the record keeps what was asked
    r = psram_speed_rec_default();
    CHECK(psram_speed_boot_decide(&r, 80, false, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_NONE && r.want == 120);
}

// #353: a restart, an OTA reboot, a panic or a watchdog at 120 proves nothing about the PSRAM: 120 stays
static void test_no_evidence(void) {
    uint8_t const resets[] = {RST_SW, RST_PANIC, RST_INT_WDT, RST_TASK_WDT, RST_WDT, RST_EXT, RST_BROWNOUT};
    for (size_t i = 0; i < sizeof(resets); i++) {
        psram_speed_rec_t r   = psram_speed_rec_default();
        psram_fail_ctx_t  c   = at(resets[i]);
        // A clean restart (fw reboot, Power > Restart, an OTA reboot) came back at 120 through the shutdown hook
        CHECK(psram_speed_boot_decide(&r, 120, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_NONE);
        CHECK(r.want == 120 && !r.failed && r.last.kind == PSRAM_FAIL_NONE);
    }
    // A panic or watchdog runs no shutdown hook, so it comes back at 80: no failure, no arm restart (no crash loop), the
    // next clean restart goes back to 120
    uint8_t const crashes[] = {RST_PANIC, RST_INT_WDT, RST_TASK_WDT, RST_LOCKUP, RST_WDT};
    for (size_t i = 0; i < sizeof(crashes); i++) {
        psram_speed_rec_t r = psram_speed_rec_default();
        psram_fail_ctx_t  c = crashes[i] == RST_WDT ? crashed_wdt() : at(crashes[i]);
        CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_KEEP120);
        CHECK(r.want == 120 && !r.failed && r.arms == 0 && r.last.kind == PSRAM_FAIL_NONE);
    }
    // A power cycle, a brownout or a restart that ran no hook: arm and restart once
    uint8_t const others[] = {RST_WDT, RST_POWERON, RST_EXT, RST_BROWNOUT, RST_SW};
    for (size_t i = 0; i < sizeof(others); i++) {
        psram_speed_rec_t r = psram_speed_rec_default();
        psram_fail_ctx_t  c = at(others[i]);
        CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_ARM_RESTART && !r.failed);
    }
}

// #353: each kind of PSRAM evidence at 120 falls back to 80 with its reason; at 80 it changes nothing
static void test_evidence(void) {
    char             t[112];
    psram_fail_ctx_t c = at(RST_SW);
    c.wall_s           = 1762869785u;
    c.uptime_s         = 42;

    psram_speed_rec_t r = psram_speed_rec_default();
    CHECK(psram_speed_boot_decide(&r, 120, true, PSRAM_FAIL_BOOT_CHECK, &c) == PSRAM_SPEED_ACT_FALL80);
    CHECK(r.want == 80 && r.failed && r.last.kind == PSRAM_FAIL_BOOT_CHECK && r.last.at120 && r.last.reset == RST_SW);
    psram_speed_fail_text(&r.last, t, sizeof(t));
    CHECK(strcmp(t, "boot check, reset sw, ran at 120, 2025-11-11 14:03:05 UTC, fw-605935b1") == 0);

    psram_fail_kind_t const later[]  = {PSRAM_FAIL_CANARY, PSRAM_FAIL_HEAP, PSRAM_FAIL_DMA_GATE};
    char const *const       texts[] = {"canary flip", "corrupt PSRAM heap", "DMA gate"};
    for (size_t i = 0; i < 3; i++) {
        r = psram_speed_rec_default();
        CHECK(psram_speed_evidence(&r, 120, later[i], &c) == PSRAM_SPEED_ACT_FALL80);
        CHECK(r.want == 80 && r.failed && r.arms == 0 && r.last.kind == later[i] && r.last.uptime_s == 42);
        CHECK(strcmp(psram_speed_fail_kind_text(r.last.kind), texts[i]) == 0);
        // the first fall stays the one recorded
        CHECK(psram_speed_evidence(&r, 120, PSRAM_FAIL_HEAP, &c) == PSRAM_SPEED_ACT_NONE && r.last.kind == later[i]);
    }
    // At 80 there is nothing to fall to
    r = psram_speed_rec_default();
    CHECK(psram_speed_evidence(&r, 80, PSRAM_FAIL_CANARY, &c) == PSRAM_SPEED_ACT_NONE && !r.failed && r.want == 120);
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_BOOT_CHECK, &c) == PSRAM_SPEED_ACT_ARM_RESTART && !r.failed);
    // Not a kind of evidence: nothing
    CHECK(psram_speed_evidence(&r, 120, PSRAM_FAIL_RETIRED_4, &c) == PSRAM_SPEED_ACT_NONE && !r.failed);

    // After the fall it stays at 80: no arm, a 120 MHz boot against the record leaves it again
    r = psram_speed_rec_default();
    psram_speed_evidence(&r, 120, PSRAM_FAIL_CANARY, &c);
    psram_fail_ctx_t on = at(RST_WDT);
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_NONE && r.failed);
    CHECK(psram_speed_boot_decide(&r, 120, true, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_LEAVE120);
    CHECK(r.last.kind == PSRAM_FAIL_CANARY); // no note of its own
    CHECK(psram_speed_leave_120_wanted(&r, 120) && !psram_speed_leave_120_wanted(&r, 80));
    r = psram_speed_rec_default();
    CHECK(!psram_speed_leave_120_wanted(&r, 120));
}

// #353: the person switches back: straight to 120 at the next boot, no trial; the same evidence rule applies
static void test_switch_back(void) {
    psram_fail_ctx_t  c = at(RST_SW);
    psram_speed_rec_t r = psram_speed_rec_default();
    psram_speed_evidence(&r, 120, PSRAM_FAIL_DMA_GATE, &c);
    psram_speed_choose(&r, 120);
    CHECK(r.want == 120 && !r.failed && r.arms == 0 && r.last.kind == PSRAM_FAIL_DMA_GATE); // the last fall is history
    CHECK(psram_speed_boot_decide(&r, 120, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_NONE && !r.failed);
    CHECK(psram_speed_boot_decide(&r, 120, true, PSRAM_FAIL_BOOT_CHECK, &c) == PSRAM_SPEED_ACT_FALL80 && r.failed);
    // and to 80 by choice: no arm, a 120 boot leaves
    psram_speed_choose(&r, 80);
    CHECK(r.want == 80 && !r.failed);
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_NONE);
    CHECK(psram_speed_boot_decide(&r, 120, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_LEAVE120);
}

// Arm restarts from 80 are bounded (a brownout at 120, a restart before the hook): no fall, this session stays at 80
static void test_arms(void) {
    psram_fail_ctx_t  brown = at(RST_BROWNOUT);
    psram_speed_rec_t r     = psram_speed_rec_default();
    for (int i = 1; i <= PSRAM_SPEED_MAX_ARMS; i++) {
        CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &brown) == PSRAM_SPEED_ACT_ARM_RESTART);
        CHECK(psram_speed_arm_commit(&r, save_ok) && r.arms == i && saved.arms == i);
    }
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &brown) == PSRAM_SPEED_ACT_KEEP120);
    CHECK(r.arms == 0 && r.want == 120 && !r.failed && r.last.kind == PSRAM_FAIL_NONE);
    // A 120 MHz boot that stays up clears the count; one that has none needs no write
    r.arms = 2;
    CHECK(!psram_speed_settle_wanted(&r, 120, PSRAM_SPEED_SETTLE_SECONDS - 1));
    CHECK(psram_speed_settle_wanted(&r, 120, PSRAM_SPEED_SETTLE_SECONDS));
    CHECK(!psram_speed_settle_wanted(&r, 80, PSRAM_SPEED_SETTLE_SECONDS));
    r.arms = 0;
    CHECK(!psram_speed_settle_wanted(&r, 120, 1000));
    // L8: a crash with arms counted keeps the session at 80 and leaves the count for the next 120 MHz boot to settle
    r                  = psram_speed_rec_default();
    r.arms             = 2;
    psram_fail_ctx_t p = at(RST_PANIC);
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &p) == PSRAM_SPEED_ACT_KEEP120 && r.arms == 2 && !r.failed);
    // L8: power lost between the arms + 1 save and the restart (or before the 120 boot settled): the next cold boot
    // (reason 7, no dump) arms again, counted, and the cap still ends it
    r                   = psram_speed_rec_default();
    psram_fail_ctx_t on = at(RST_WDT);
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_ARM_RESTART);
    CHECK(psram_speed_arm_commit(&r, save_ok) && saved.arms == 1);
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_ARM_RESTART && r.arms == 1);
    CHECK(psram_speed_arm_commit(&r, save_ok) && saved.arms == 2);
    CHECK(psram_speed_arm_commit(&r, save_ok) && saved.arms == 3);
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &on) == PSRAM_SPEED_ACT_KEEP120 && r.arms == 0);
    // A refused save arms nothing and wants 80 this session
    r = psram_speed_rec_default();
    CHECK(!psram_speed_arm_commit(&r, save_fail) && r.want == 80 && r.arms == 0);
}

static void test_texts(void) {
    char              t[48];
    psram_speed_rec_t r = {.want = 80};
    psram_speed_value_text(&r, 80, t, sizeof(t));
    CHECK(strcmp(t, "80 MHz") == 0);
    r = psram_speed_rec_default();
    psram_speed_value_text(&r, 120, t, sizeof(t));
    CHECK(strcmp(t, "120 MHz") == 0);
    psram_speed_value_text(&r, 80, t, sizeof(t));
    CHECK(strcmp(t, "80 MHz, switching to 120") == 0);
    r = (psram_speed_rec_t){.want = 80, .failed = 1};
    psram_speed_value_text(&r, 80, t, sizeof(t));
    CHECK(strcmp(t, "120 MHz failed, back at 80") == 0);
    r = psram_speed_rec_default();
    psram_speed_fail_text(&r.last, t, sizeof(t));
    CHECK(strcmp(t, "none") == 0);
    CHECK(strcmp(psram_speed_reset_name(15), "cpu_lockup") == 0 && strcmp(psram_speed_reset_name(16), "?") == 0);
    CHECK(strcmp(psram_speed_fail_kind_text(PSRAM_FAIL_RETIRED_3), "?") == 0);
    // A long image version is cut at 15
    psram_fail_ctx_t c = at(RST_SW);
    c.fw               = "fw-0123456789abcdef-dirty";
    psram_speed_fail_note(&r, PSRAM_FAIL_CANARY, &c);
    CHECK(strlen(r.last.fw) == PSRAM_FAIL_FW_MAX && strncmp(r.last.fw, "fw-0123456789ab", 15) == 0);
}

// The NVS blob: round trip, and nothing that is not a v4 record (the trial's v3 one included) read as facts
static void test_record(void) {
    uint8_t           b[PSRAM_SPEED_REC_SIZE];
    psram_speed_rec_t r = {.want = 80, .failed = 1, .arms = 2}, back;
    psram_fail_ctx_t  c = {.reset = 5, .wall_s = 1762869785u, .uptime_s = 77, .fw = "fw-605935b1"};
    psram_speed_fail_note(&r, PSRAM_FAIL_HEAP, &c);
    psram_speed_rec_encode(&r, b);
    CHECK(b[4] == PSRAM_SPEED_REC_VERSION && b[31] == 0 && b[3] == 0);
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(memcmp(&r, &back, sizeof(r)) == 0);

    // The trial's 36-byte v3 record (#329: want, trial, failed, aborts, version 3, ...). A trial-era fall (want 80,
    // failed) was no PSRAM evidence: 120. want 80 without a fall was the person's choice and stays (review L4)
    uint8_t v3[36] = {80, 0, 1, 0, 3, 4, 3, 1};
    psram_speed_rec_decode(v3, sizeof(v3), &back);
    CHECK(back.want == 120 && !back.failed && back.arms == 0 && back.last.kind == PSRAM_FAIL_NONE);
    CHECK(back.last.fw[0] == 0 && back.last.wall_s == 0 && back.last.uptime_s == 0 && back.last.reset == 0);
    uint8_t chose80[36] = {80, 0, 0, 0, 3};
    psram_speed_rec_decode(chose80, sizeof(chose80), &back);
    CHECK(back.want == 80 && !back.failed && back.arms == 0 && back.last.kind == PSRAM_FAIL_NONE);
    uint8_t chose120[36] = {120, 1, 0, 2, 3};
    psram_speed_rec_decode(chose120, sizeof(chose120), &back);
    CHECK(back.want == 120 && !back.failed && back.arms == 0);
    chose80[4] = 2; // not v3: the default
    psram_speed_rec_decode(chose80, sizeof(chose80), &back);
    CHECK(back.want == 120);
    // Another size or version: the default, not garbage
    psram_speed_rec_encode(&r, b);
    b[4] = PSRAM_SPEED_REC_VERSION + 1;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 120 && !back.failed && back.last.kind == PSRAM_FAIL_NONE);
    psram_speed_rec_encode(&r, b);
    psram_speed_rec_decode(b, 7, &back);
    CHECK(back.want == 120 && back.last.kind == PSRAM_FAIL_NONE);

    // A current blob with a retired or unknown kind keeps its speed state, the failure reads as none, facts cleared
    psram_speed_rec_encode(&r, b);
    b[5] = PSRAM_FAIL_RETIRED_4;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 80 && back.failed && back.arms == 2 && back.last.kind == PSRAM_FAIL_NONE && back.last.uptime_s == 0);
    b[5] = PSRAM_FAIL_KINDS;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.last.kind == PSRAM_FAIL_NONE);
    // Non-text bytes in the version are not printed; too many arms or a bad flag is foreign
    psram_speed_rec_encode(&r, b);
    b[17] = 0x01;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(strcmp(back.last.fw, "f?-605935b1") == 0);
    psram_speed_rec_encode(&r, b);
    b[2] = PSRAM_SPEED_MAX_ARMS + 1;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 120 && back.arms == 0 && !back.failed && back.last.kind == PSRAM_FAIL_NONE);
    psram_speed_rec_encode(&r, b);
    b[1] = 2;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 120 && !back.failed);
}

// #353 review H1: on this board reason 7 (ESP_RST_WDT) is also every power-on and CH340 reset; it is a crash only with a
// core dump new since the last recorded crash, as crash.c judges it. Panic, int / task watchdog and lockup always are.
static void test_crash_class(void) {
    CHECK(!psram_speed_reset_crashed(RST_WDT, false) && psram_speed_reset_crashed(RST_WDT, true));
    CHECK(psram_speed_reset_crashed(RST_PANIC, false) && psram_speed_reset_crashed(RST_INT_WDT, false));
    CHECK(psram_speed_reset_crashed(RST_TASK_WDT, false) && psram_speed_reset_crashed(RST_LOCKUP, false));
    CHECK(!psram_speed_reset_crashed(RST_SW, true) && !psram_speed_reset_crashed(RST_POWERON, true));
    CHECK(!psram_speed_reset_crashed(RST_BROWNOUT, true) && !psram_speed_reset_crashed(RST_EXT, false));
    psram_speed_rec_t r  = psram_speed_rec_default();
    psram_fail_ctx_t  c7 = at(RST_WDT); // a cold boot: arm and restart to 120
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &c7) == PSRAM_SPEED_ACT_ARM_RESTART);
    r                      = psram_speed_rec_default();
    psram_fail_ctx_t wdt   = crashed_wdt(); // a real watchdog: the crash path, this session at 80
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &wdt) == PSRAM_SPEED_ACT_KEEP120 && !r.failed);
}

// #353 review L2: a fall whose record could not be saved travels in STORE15 to the next boot, which runs at 80 (any
// word but the ARM word), records it there and does not arm into the suspect PSRAM
static void test_carried_fall(void) {
    CHECK(psram_speed_fall_word(PSRAM_FAIL_CANARY) != PSRAM_SPEED_ARM_WORD);
    CHECK(psram_speed_carried_fall(psram_speed_fall_word(PSRAM_FAIL_CANARY)) == PSRAM_FAIL_CANARY);
    CHECK(psram_speed_carried_fall(psram_speed_fall_word(PSRAM_FAIL_HEAP)) == PSRAM_FAIL_HEAP);
    CHECK(psram_speed_carried_fall(PSRAM_SPEED_ARM_WORD) == PSRAM_FAIL_NONE);
    CHECK(psram_speed_carried_fall(0) == PSRAM_FAIL_NONE);
    CHECK(psram_speed_carried_fall(psram_speed_fall_word(PSRAM_FAIL_RETIRED_5)) == PSRAM_FAIL_NONE);
    psram_speed_rec_t r = psram_speed_rec_default(); // the flash still says 120: the save failed
    psram_fail_ctx_t  c = at(RST_SW);
    c.carried           = PSRAM_FAIL_CANARY;
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_NONE);
    CHECK(r.want == 80 && r.failed && r.last.kind == PSRAM_FAIL_CANARY && r.last.at120);
    // a carried fall on a record that already says 80 changes nothing
    psram_speed_rec_t before = r;
    CHECK(psram_speed_boot_decide(&r, 80, true, PSRAM_FAIL_NONE, &c) == PSRAM_SPEED_ACT_NONE);
    CHECK(memcmp(&before, &r, sizeof(r)) == 0);
}
