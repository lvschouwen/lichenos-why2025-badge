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

int main(void) {
    bool live;
    psram_speed_rec_t r = {0};
    psram_speed_rec_sane(&r); // an empty or foreign record is the default
    CHECK(r.want == 80 && !r.trial && !r.failed);
    r = (psram_speed_rec_t){.want = 100};
    psram_speed_rec_sane(&r);
    CHECK(r.want == 80);

    // Default: at 80 nothing happens, not even with the IDF unpatched
    CHECK(psram_speed_boot_decide(&r, 80, true, &live) == PSRAM_SPEED_ACT_NONE && !live && !r.trial);
    CHECK(psram_speed_boot_decide(&r, 80, false, &live) == PSRAM_SPEED_ACT_NONE);

    // The user chooses 120: armed at 80 (trial set before the restart), then the 120 boot is a live trial that passes
    psram_speed_choose(&r, 120);
    CHECK(r.want == 120 && r.trial && !r.failed);
    CHECK(psram_speed_boot_decide(&r, 120, true, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial);
    CHECK(psram_speed_trial_step(10, false, false) == PSRAM_SPEED_TRIAL_RUNNING);
    CHECK(psram_speed_trial_step(PSRAM_SPEED_TRIAL_SECONDS, false, false) == PSRAM_SPEED_TRIAL_PASS);
    psram_speed_pass(&r);
    CHECK(r.want == 120 && !r.trial && !r.failed);

    // A crash after it passed: the register is clear, boot at 80, the record still wants 120: armed once more
    CHECK(psram_speed_boot_decide(&r, 80, true, &live) == PSRAM_SPEED_ACT_ARM_RESTART && r.trial);
    // ...and that 120 boot crashes before it passes: next boot at 80 with the trial still pending = failed, never re-armed
    CHECK(psram_speed_boot_decide(&r, 80, true, &live) == PSRAM_SPEED_ACT_NONE && !live);
    CHECK(r.want == 80 && r.failed && !r.trial);
    CHECK(psram_speed_boot_decide(&r, 80, true, &live) == PSRAM_SPEED_ACT_NONE && !r.trial); // stays at 80 for good
    CHECK(psram_speed_boot_decide(&r, 80, true, &live) == PSRAM_SPEED_ACT_NONE);

    // Choosing 120 again clears the failed flag
    psram_speed_choose(&r, 120);
    CHECK(!r.failed && r.want == 120);
    // Choosing 80 clears everything
    psram_speed_choose(&r, 80);
    CHECK(r.want == 80 && !r.trial && !r.failed);

    // A clean restart at 120 (register kept armed): the record has trial 0 -> a new live trial
    r = (psram_speed_rec_t){.want = 120};
    CHECK(psram_speed_boot_decide(&r, 120, true, &live) == PSRAM_SPEED_ACT_NONE && live && r.trial);

    // A bad boot check, a failed gate or a fallback tuning fails the trial at once
    CHECK(psram_speed_trial_step(5, true, false) == PSRAM_SPEED_TRIAL_FAIL);
    CHECK(psram_speed_trial_step(5, false, true) == PSRAM_SPEED_TRIAL_FAIL);
    CHECK(psram_speed_trial_step(500, true, false) == PSRAM_SPEED_TRIAL_FAIL); // bad beats the clock
    psram_speed_fail(&r);
    CHECK(r.want == 80 && r.failed && !r.trial);

    // 120 running against the record (after a fail, or want 80): restart, which lands at 80
    r = (psram_speed_rec_t){.want = 80};
    CHECK(psram_speed_boot_decide(&r, 120, true, &live) == PSRAM_SPEED_ACT_RESTART && !live && r.want == 80);

    // IDF not patched: a record that wants 120 stays as it is and never restarts
    r = (psram_speed_rec_t){.want = 120, .trial = 1};
    CHECK(psram_speed_boot_decide(&r, 80, false, &live) == PSRAM_SPEED_ACT_NONE && r.want == 120 && r.trial);

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

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_speed_test ok\n");
    return 0;
}
