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

int main(void) {
    bool live;
    psram_speed_rec_t r = {0};
    psram_speed_rec_sane(&r); // an empty or foreign record is the default
    CHECK(r.want == 80 && !r.trial && !r.failed);
    r = (psram_speed_rec_t){.want = 100};
    psram_speed_rec_sane(&r);
    CHECK(r.want == 80);

    // Default: at 80 nothing happens, not even with the IDF unpatched
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &live) == PSRAM_SPEED_ACT_NONE && !live && !r.trial);
    CHECK(psram_speed_boot_decide(&r, 80, false, false, &live) == PSRAM_SPEED_ACT_NONE);

    // The user chooses 120: armed at 80 (trial set before the restart), then the 120 boot is a live trial that passes
    psram_speed_choose(&r, 120);
    CHECK(r.want == 120 && r.trial && !r.failed);
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial);
    CHECK(psram_speed_trial_step(10, false, false) == PSRAM_SPEED_TRIAL_RUNNING);
    CHECK(psram_speed_trial_step(PSRAM_SPEED_TRIAL_SECONDS, false, false) == PSRAM_SPEED_TRIAL_PASS);
    psram_speed_pass(&r);
    CHECK(r.want == 120 && !r.trial && !r.failed);

    // A crash after it passed: the register is clear, boot at 80, the record still wants 120: armed once more
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &live) == PSRAM_SPEED_ACT_ARM_RESTART);
    CHECK(psram_speed_arm_commit(&r, save_ok) && r.trial); // what the glue does before it arms
    // ...and that 120 boot crashes before it passes: next boot at 80 with the trial still pending = failed, never re-armed
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &live) == PSRAM_SPEED_ACT_NONE && !live);
    CHECK(r.want == 80 && r.failed && !r.trial);
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // stays at 80 for good
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &live) == PSRAM_SPEED_ACT_NONE);

    // Choosing 120 again clears the failed flag
    psram_speed_choose(&r, 120);
    CHECK(!r.failed && r.want == 120);
    // Choosing 80 clears everything
    psram_speed_choose(&r, 80);
    CHECK(r.want == 80 && !r.trial && !r.failed);

    // A clean restart at 120 (register kept armed): the record has trial 0 -> a new live trial
    r = (psram_speed_rec_t){.want = 120};
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial);

    // A bad boot check, a failed gate or a fallback tuning fails the trial at once
    CHECK(psram_speed_trial_step(5, true, false) == PSRAM_SPEED_TRIAL_FAIL);
    CHECK(psram_speed_trial_step(5, false, true) == PSRAM_SPEED_TRIAL_FAIL);
    CHECK(psram_speed_trial_step(500, true, false) == PSRAM_SPEED_TRIAL_FAIL); // bad beats the clock
    psram_speed_fail(&r);
    CHECK(r.want == 80 && r.failed && !r.trial);

    // 120 running against the record (after a fail, or want 80): restart, which lands at 80
    r = (psram_speed_rec_t){.want = 80};
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &live) == PSRAM_SPEED_ACT_RESTART && !live && r.want == 80);

    // IDF not patched: a record that wants 120 stays as it is and never restarts
    r = (psram_speed_rec_t){.want = 120, .trial = 1};
    CHECK(psram_speed_boot_decide(&r, 80, false, false, &live) == PSRAM_SPEED_ACT_NONE && r.want == 120 && r.trial);

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
    CHECK(psram_speed_boot_decide(&r, 80, true, true, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // arm deferred
    CHECK(!psram_speed_arm_wanted(&r, 80, true, true));
    CHECK(psram_speed_arm_wanted(&r, 80, true, false)); // the tick, once the image is valid
    CHECK(!psram_speed_arm_wanted(&r, 80, false, false)); // IDF not patched
    CHECK(!psram_speed_arm_wanted(&r, 120, true, false));
    r = (psram_speed_rec_t){.want = 120, .failed = 1};
    CHECK(!psram_speed_arm_wanted(&r, 80, true, false));
    r = (psram_speed_rec_t){.want = 80};
    CHECK(psram_speed_boot_decide(&r, 120, true, true, &live) == PSRAM_SPEED_ACT_NONE && r.failed); // no restart while pending
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
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial); // trial begins
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &live) == PSRAM_SPEED_ACT_NONE && r.failed && r.want == 80); // restart mid-trial
    CHECK(psram_speed_boot_decide(&r, 80, true, false, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // and no second attempt
    // after a pass a clean restart (register re-armed by the hook) starts a new trial, not a failure
    r = (psram_speed_rec_t){.want = 120, .trial = 0};
    CHECK(psram_speed_boot_decide(&r, 120, true, false, &live) == PSRAM_SPEED_ACT_NONE && live);

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

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_speed_test ok\n");
    return 0;
}
