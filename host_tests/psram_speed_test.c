// Host test: the PSRAM 80/120 MHz switch state machine (#121)
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

// The judging boot's facts: a panic reset, clock not set
static psram_fail_ctx_t ctx = {.reset = 4, .wall_s = 0, .uptime_s = 3, .fw = "fw-605935b1"};

static void test_fail_kinds(void);
static void test_record(void);

int main(void) {
    bool live;
    psram_speed_rec_t r = {0};
    psram_speed_rec_sane(&r); // an empty or foreign record is the default
    CHECK(r.want == 80 && !r.trial && !r.failed);
    r = (psram_speed_rec_t){.want = 100};
    psram_speed_rec_sane(&r);
    CHECK(r.want == 80);

    // Default: at 80 nothing happens, not even with the IDF unpatched
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && !live && !r.trial);
    CHECK(psram_speed_boot_decide(&r, 80, false, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE);

    // The user chooses 120: armed at 80 (trial set before the restart), then the 120 boot is a live trial that passes
    psram_speed_choose(&r, 120);
    CHECK(r.want == 120 && r.trial && !r.failed);
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial);
    CHECK(psram_speed_trial_step(10, false, false) == PSRAM_SPEED_TRIAL_RUNNING);
    CHECK(psram_speed_trial_step(PSRAM_SPEED_TRIAL_SECONDS, false, false) == PSRAM_SPEED_TRIAL_PASS);
    psram_speed_pass(&r);
    CHECK(r.want == 120 && !r.trial && !r.failed);

    // A crash after it passed: the register is clear, boot at 80, the record still wants 120: armed once more
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_ARM_RESTART);
    CHECK(psram_speed_arm_commit(&r, save_ok) && r.trial); // what the glue does before it arms
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && live); // the 120 MHz boot
    // ...and that 120 boot crashes before it passes: next boot at 80 with the trial still pending = failed, never re-armed
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && !live);
    CHECK(r.want == 80 && r.failed && !r.trial);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // stays at 80 for good
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE);

    // Choosing 120 again clears the failed flag
    psram_speed_choose(&r, 120);
    CHECK(!r.failed && r.want == 120);
    // Choosing 80 clears everything
    psram_speed_choose(&r, 80);
    CHECK(r.want == 80 && !r.trial && !r.failed);

    // A clean restart at 120 (register kept armed): the record has trial 0 -> a new live trial
    r = (psram_speed_rec_t){.want = 120};
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial);

    // A bad boot check, a failed gate or a fallback tuning fails the trial at once
    CHECK(psram_speed_trial_step(5, true, false) == PSRAM_SPEED_TRIAL_FAIL);
    CHECK(psram_speed_trial_step(5, false, true) == PSRAM_SPEED_TRIAL_FAIL);
    CHECK(psram_speed_trial_step(500, true, false) == PSRAM_SPEED_TRIAL_FAIL); // bad beats the clock
    psram_speed_fail(&r);
    CHECK(r.want == 80 && r.failed && !r.trial);

    // 120 running against the record (after a fail, or want 80): restart, which lands at 80
    r = (psram_speed_rec_t){.want = 80};
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &ctx, &live) == PSRAM_SPEED_ACT_RESTART && !live && r.want == 80);

    // IDF not patched: a record that wants 120 stays as it is and never restarts
    r = (psram_speed_rec_t){.want = 120, .trial = 1};
    CHECK(psram_speed_boot_decide(&r, 80, false, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && r.want == 120 && r.trial);

    // Texts
    char t[48];
    r = (psram_speed_rec_t){.want = 80};
    psram_speed_value_text(&r, 80, t, sizeof(t));
    CHECK(strcmp(t, "80 MHz") == 0);
    r = (psram_speed_rec_t){.want = 120, .trial = 1};
    psram_speed_value_text(&r, 120, t, sizeof(t));
    CHECK(strcmp(t, "120 MHz, testing") == 0);
    r.trial = 0;
    psram_speed_value_text(&r, 120, t, sizeof(t));
    CHECK(strcmp(t, "120 MHz") == 0);
    r = (psram_speed_rec_t){.want = 80, .failed = 1};
    psram_speed_value_text(&r, 80, t, sizeof(t));
    CHECK(strcmp(t, "120 MHz failed, back at 80") == 0);

    // H1: an image that is still PENDING_VERIFY is never restarted: no arm, a fail only records itself
    r = (psram_speed_rec_t){.want = 120};
    CHECK(psram_speed_boot_decide(&r, 80, true, true, &ctx, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // arm deferred
    CHECK(!psram_speed_arm_wanted(&r, 80, true, true));
    CHECK(psram_speed_arm_wanted(&r, 80, true, false)); // the tick, once the image is valid
    CHECK(!psram_speed_arm_wanted(&r, 80, false, false)); // IDF not patched
    CHECK(!psram_speed_arm_wanted(&r, 120, true, false));
    r = (psram_speed_rec_t){.want = 120, .failed = 1};
    CHECK(!psram_speed_arm_wanted(&r, 80, true, false));
    r = (psram_speed_rec_t){.want = 80};
    CHECK(psram_speed_boot_decide(&r, 120, true, true, &ctx, &live) == PSRAM_SPEED_ACT_NONE && r.failed); // no restart while pending
    CHECK(!psram_speed_leave_120_wanted(&r, 120, true));
    CHECK(psram_speed_leave_120_wanted(&r, 120, false)); // later, once valid
    CHECK(!psram_speed_leave_120_wanted(&r, 80, false));
    r = (psram_speed_rec_t){.want = 120};
    CHECK(!psram_speed_leave_120_wanted(&r, 120, false));

    // H2: the record is saved before anything is armed; a refused save arms nothing and wants 80 in RAM
    r = (psram_speed_rec_t){.want = 120};
    CHECK(psram_speed_arm_commit(&r, save_ok) && r.trial && r.want == 120 && saved.trial == 1);
    r = (psram_speed_rec_t){.want = 120};
    CHECK(!psram_speed_arm_commit(&r, save_fail) && r.want == 80 && !r.trial);

    // M1: restarts during a trial: the trial is still pending on the next boot, which is then at 80 = failed
    r = (psram_speed_rec_t){.want = 120};
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial); // trial begins
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && r.failed && r.want == 80); // restart mid-trial
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // and no second attempt
    // after a pass a clean restart (register re-armed by the hook) starts a new trial, not a failure
    r = (psram_speed_rec_t){.want = 120, .trial = 0};
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && live);

    // Lows: strict command parse
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

    test_fail_kinds();
    test_record();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_speed_test ok\n");
    return 0;
}

// #329: every way a trial ends maps to its record; a crash before the switch took effect is an abort, not a failure
static void test_fail_kinds(void) {
    bool              live;
    psram_speed_rec_t r;
    char              t[96];

    // The #329 case: chosen at 80 (ARMED), a panic on the way into the trial, the next boot is still at 80
    r = (psram_speed_rec_t){.want = 80};
    psram_speed_choose(&r, 120);
    CHECK(r.trial == PSRAM_SPEED_TRIAL_ARMED);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_REARM && !live);
    CHECK(!r.failed && r.want == 120 && r.trial == PSRAM_SPEED_TRIAL_ARMED && r.aborts == 1); // still armed
    CHECK(r.last.kind == PSRAM_FAIL_ABORTED && r.last.reset == 4 && !r.last.at120 && r.last.uptime_s == 3);
    CHECK(strcmp(r.last.fw, "fw-605935b1") == 0 && r.last.wall_s == 0);
    psram_speed_fail_text(&r.last, t, sizeof(t));
    CHECK(strcmp(t, "aborted (ran at 80), reset panic, ran at 80, up 3 s, fw-605935b1") == 0);
    psram_speed_value_text(&r, 80, t, sizeof(t));
    CHECK(strcmp(t, "80 MHz, switching to 120") == 0);
    CHECK(!psram_speed_arm_wanted(&r, 80, true, false)); // no restart of its own: the next clean restart tries 120

    // The clean restart does reach 120: the trial now RAN (saved), the abort count starts over, and it passes
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && live);
    CHECK(r.trial == PSRAM_SPEED_TRIAL_RAN && r.aborts == 0);
    psram_speed_pass(&r);
    CHECK(!r.trial && !r.failed && r.want == 120 && r.last.kind == PSRAM_FAIL_ABORTED); // history kept

    // Three aborts in a row: given up, failed, recorded as such
    psram_speed_choose(&r, 120);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_REARM && r.aborts == 1);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_REARM && r.aborts == 2);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE);
    CHECK(r.failed && r.want == 80 && !r.trial && r.last.kind == PSRAM_FAIL_ABORTS && !r.last.at120);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &ctx, &live) == PSRAM_SPEED_ACT_NONE && r.failed); // stays
    psram_speed_choose(&r, 120);
    CHECK(r.aborts == 0 && !r.failed);

    // A crash while running 120 (trial RAN): failed as before, crash by the reset reason, ran at 120
    r = (psram_speed_rec_t){.want = 120, .trial = PSRAM_SPEED_TRIAL_RAN};
    psram_fail_ctx_t wdt = {.reset = 6, .wall_s = 1762869785u, .uptime_s = 2, .fw = "fw-abcdef12"}; // task_wdt
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &wdt, &live) == PSRAM_SPEED_ACT_NONE);
    CHECK(r.failed && r.want == 80 && !r.trial && r.last.kind == PSRAM_FAIL_CRASH && r.last.at120 && r.last.reset == 6);
    psram_speed_fail_text(&r.last, t, sizeof(t));
    CHECK(strcmp(t, "crash or watchdog, reset task_wdt, ran at 120, 2025-11-11 14:03:05 UTC, fw-abcdef12") == 0);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &wdt, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // never again

    // A plain restart or power loss during a 120 MHz trial: failed, "restart before it passed"
    r                       = (psram_speed_rec_t){.want = 120, .trial = PSRAM_SPEED_TRIAL_RAN};
    psram_fail_ctx_t sw     = {.reset = 3, .wall_s = 1000, .uptime_s = 4, .fw = "fw-0123456789abcdef-dirty"};
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &sw, &live) == PSRAM_SPEED_ACT_NONE && r.failed);
    CHECK(r.last.kind == PSRAM_FAIL_RESTART && r.last.wall_s == 0); // a clock before 2025 is not set
    CHECK(strlen(r.last.fw) == PSRAM_FAIL_FW_MAX && strncmp(r.last.fw, "fw-0123456789ab", 15) == 0);
    psram_speed_fail_text(&r.last, t, sizeof(t));
    CHECK(strcmp(t, "restart before it passed, reset sw, ran at 120, up 4 s, fw-0123456789ab") == 0);

    // The live trial's own checks (glue: fail_and_restart at 120)
    r = (psram_speed_rec_t){.want = 120, .trial = PSRAM_SPEED_TRIAL_RAN};
    psram_speed_fail_at(&r, PSRAM_FAIL_BOOT_CHECK, &sw, true);
    CHECK(r.failed && r.want == 80 && r.last.kind == PSRAM_FAIL_BOOT_CHECK && r.last.at120);
    psram_speed_fail_at(&r, PSRAM_FAIL_DMA_GATE, &sw, true);
    CHECK(r.last.kind == PSRAM_FAIL_DMA_GATE);
    psram_speed_fail_text(&r.last, t, sizeof(t));
    CHECK(strncmp(t, "DMA gate, reset sw, ran at 120, ", 32) == 0);

    // Names
    CHECK(strcmp(psram_speed_reset_name(15), "cpu_lockup") == 0 && strcmp(psram_speed_reset_name(16), "?") == 0);
    CHECK(psram_speed_reset_is_crash(4) && psram_speed_reset_is_crash(5) && psram_speed_reset_is_crash(7));
    CHECK(psram_speed_reset_is_crash(15) && !psram_speed_reset_is_crash(3) && !psram_speed_reset_is_crash(9));
    r = (psram_speed_rec_t){.want = 80};
    psram_speed_fail_text(&r.last, t, sizeof(t));
    CHECK(strcmp(t, "none") == 0);
}

// The NVS blob: round trip, the v1 record of older images, and nothing foreign read as facts
static void test_record(void) {
    uint8_t           b[PSRAM_SPEED_REC_SIZE];
    psram_speed_rec_t r = {.want = 120, .trial = PSRAM_SPEED_TRIAL_ARMED, .aborts = 2}, back;
    psram_fail_ctx_t  c = {.reset = 5, .wall_s = 1762869785u, .uptime_s = 77, .fw = "fw-605935b1"};
    psram_speed_fail_note(&r, PSRAM_FAIL_ABORTED, &c, false);
    psram_speed_rec_encode(&r, b);
    CHECK(b[4] == PSRAM_SPEED_REC_VERSION && b[31] == 0);
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(memcmp(&r, &back, sizeof(r)) == 0);

    // v1 (4 bytes, before #329): the speed state, no failure recorded, a stray pad byte is not an abort count
    uint8_t v1[4] = {80, 0, 1, 7};
    psram_speed_rec_decode(v1, sizeof(v1), &back);
    CHECK(back.want == 80 && back.failed && !back.trial && back.aborts == 0 && back.last.kind == PSRAM_FAIL_NONE);
    CHECK(back.last.fw[0] == 0 && back.last.wall_s == 0 && back.last.uptime_s == 0 && back.last.reset == 0);
    uint8_t v1b[4] = {120, 1, 0, 0};
    psram_speed_rec_decode(v1b, sizeof(v1b), &back);
    CHECK(back.want == 120 && back.trial == PSRAM_SPEED_TRIAL_ARMED && !back.failed);

    // Another size or version: the default, not garbage
    b[4] = 3;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 80 && !back.trial && back.last.kind == PSRAM_FAIL_NONE);
    psram_speed_rec_decode(b, 7, &back);
    CHECK(back.want == 80 && back.last.kind == PSRAM_FAIL_NONE);

    // A v2 blob with a bad failure kind keeps its speed state, the failure reads as none with every fact cleared
    psram_speed_rec_encode(&r, b);
    b[5] = PSRAM_FAIL_KINDS;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 120 && back.aborts == 2 && back.last.kind == PSRAM_FAIL_NONE && back.last.uptime_s == 0);
    // Non-text bytes in the version are not printed; too many aborts or an unknown trial state is foreign
    psram_speed_rec_encode(&r, b);
    b[17] = 0x01;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(strcmp(back.last.fw, "f?-605935b1") == 0);
    b[3] = PSRAM_SPEED_MAX_ABORTS + 1;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 80 && back.aborts == 0 && back.last.kind == PSRAM_FAIL_NONE);
    psram_speed_rec_encode(&r, b);
    b[1] = 3;
    psram_speed_rec_decode(b, sizeof(b), &back);
    CHECK(back.want == 80 && !back.trial);
}
