// Host test: whether a restart waits for the running image's valid mark (#304)
#include "ota_valid.h"

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

// An automatic restart: (asked = false), not settled, no power off
static restart_gate_t automatic(ota_image_state_t image, uint32_t uptime_s) {
    return restart_gate_decide(false, image, false, uptime_s, false);
}

// Not on trial: nothing to wait for, whatever else holds
static void test_not_on_trial(void) {
    CHECK(automatic(OTA_IMAGE_NOT_ON_TRIAL, 0) == RESTART_GATE_GO);
    CHECK(restart_gate_decide(true, OTA_IMAGE_NOT_ON_TRIAL, false, 0, false) == RESTART_GATE_GO);
    CHECK(restart_gate_decide(false, OTA_IMAGE_NOT_ON_TRIAL, true, 500, true) == RESTART_GATE_GO);
}

// On trial (or not read yet): an automatic restart waits until the bound
static void test_waits_on_trial(void) {
    CHECK(automatic(OTA_IMAGE_ON_TRIAL, 0) == RESTART_GATE_WAIT);
    CHECK(automatic(OTA_IMAGE_ON_TRIAL, OTA_VALID_AFTER_S) == RESTART_GATE_WAIT);
    CHECK(automatic(OTA_IMAGE_ON_TRIAL, OTA_RESTART_GATE_MAX_S - 1) == RESTART_GATE_WAIT);
    CHECK(automatic(OTA_IMAGE_UNKNOWN, 10) == RESTART_GATE_WAIT); // an unread state might be a trial
    CHECK(automatic(OTA_IMAGE_ON_TRIAL, OTA_RESTART_GATE_MAX_S) == RESTART_GATE_GO_BOUND);
    CHECK(automatic(OTA_IMAGE_UNKNOWN, OTA_RESTART_GATE_MAX_S + 100) == RESTART_GATE_GO_BOUND);
    // The bound comes after the trial loop's own last second, so the loop has ended (or rolled back) first
    CHECK(OTA_RESTART_GATE_MAX_S > OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S);
}

// What ends the wait early
static void test_ends_early(void) {
    // A person asked, or the rollback is meant: at once
    CHECK(restart_gate_decide(true, OTA_IMAGE_ON_TRIAL, false, 0, false) == RESTART_GATE_GO_ASKED);
    CHECK(restart_gate_decide(true, OTA_IMAGE_UNKNOWN, false, 0, false) == RESTART_GATE_GO_ASKED);
    // The trial loop ended without the mark (STAY, MARK_FAILED, NO_STATE): a restart rolls back whenever it comes
    CHECK(restart_gate_decide(false, OTA_IMAGE_ON_TRIAL, true, 60, false) == RESTART_GATE_GO_SETTLED);
    // Heat never waits
    CHECK(restart_gate_decide(false, OTA_IMAGE_ON_TRIAL, false, 5, true) == RESTART_GATE_GO_POWER_OFF);
    CHECK(restart_gate_decide(false, OTA_IMAGE_ON_TRIAL, true, 5, true) == RESTART_GATE_GO_POWER_OFF);
}

static void test_names(void) {
    CHECK(strcmp(restart_gate_name(RESTART_GATE_GO), "") == 0);
    CHECK(strcmp(restart_gate_name(RESTART_GATE_WAIT), "") == 0);
    CHECK(restart_gate_name(RESTART_GATE_GO_ASKED)[0]);
    CHECK(restart_gate_name(RESTART_GATE_GO_SETTLED)[0]);
    CHECK(restart_gate_name(RESTART_GATE_GO_BOUND)[0]);
    CHECK(restart_gate_name(RESTART_GATE_GO_POWER_OFF)[0]);
}

int main(void) {
    test_not_on_trial();
    test_waits_on_trial();
    test_ends_early();
    test_names();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("restart_gate_test: all checks passed\n");
    return 0;
}
