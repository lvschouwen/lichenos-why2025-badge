// Host test: when a freshly installed lean firmware (#61) may cancel the rollback
#include "ota_valid.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_image_header(void) {
    uint8_t head[OTA_IMAGE_HEADER_LEN] = {0xE9, 5};
    head[12] = 0x12; // ESP32-P4
    CHECK(ota_image_header_ok(head, sizeof(head), 0x0012));
    CHECK(!ota_image_header_ok(head, sizeof(head), 0x000D)); // a C6 badge wants a C6 image
    CHECK(!ota_image_header_ok(head, sizeof(head) - 1, 0x0012));
    CHECK(!ota_image_header_ok(NULL, 0, 0x0012));
    head[0] = 0x50; // "PK": a zip, not an image
    CHECK(!ota_image_header_ok(head, sizeof(head), 0x0012));
    head[0]  = 0xE9;
    head[12] = 0x0D; // a C6 image pushed to the P4
    CHECK(!ota_image_header_ok(head, sizeof(head), 0x0012));
}

// A display boot with everything up, settled: the case that marks the image valid
static ota_trial_in_t up_at(uint32_t uptime_s) {
    return (ota_trial_in_t){.pending = true, .boot_done = true, .display = true, .shell_up = true, .c6_up = true,
                            .uptime_s = uptime_s};
}

static ota_trial_act_t step(ota_trial_in_t in) {
    return ota_trial_step(&in);
}

// A display boot: the rules as before #293 (the shell and the C6 link up, OTA_VALID_AFTER_S)
static void test_display_boot(void) {
    uint32_t const settled = OTA_VALID_AFTER_S;
    CHECK(step(up_at(settled)) == OTA_TRIAL_MARK_VALID);
    CHECK(step(up_at(settled + 100)) == OTA_TRIAL_MARK_VALID);

    // Not yet: a build that crashes seconds after boot must still roll back
    CHECK(step(up_at(0)) == OTA_TRIAL_WAIT);
    CHECK(step(up_at(settled - 1)) == OTA_TRIAL_WAIT);

    // The shell is not up: wait, then stay on trial (the C6 is up, so Wi-Fi can replace the image)
    ota_trial_in_t in = up_at(settled - 1);
    in.shell_up       = false;
    CHECK(step(in) == OTA_TRIAL_WAIT);
    in.uptime_s = settled;
    CHECK(step(in) == OTA_TRIAL_STAY);

    // The C6 link is not up: stay on trial at the window, roll back after the grace time
    in          = up_at(settled);
    in.c6_up    = false;
    CHECK(step(in) == OTA_TRIAL_STAY);
    in.uptime_s = OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
    CHECK(step(in) == OTA_TRIAL_ROLL_BACK);
    in.shell_up = false;
    CHECK(step(in) == OTA_TRIAL_ROLL_BACK);

    // The boot steps have not all run: never valid
    in           = up_at(settled);
    in.boot_done = false;
    CHECK(step(in) == OTA_TRIAL_STAY);
}

// #293: without a display the OS being up is the health signal; the shell is not asked
static void test_headless_boot(void) {
    uint32_t const settled = OTA_VALID_AFTER_S;
    ota_trial_in_t in      = up_at(settled);
    in.display             = false;
    in.shell_up            = false;
    CHECK(step(in) == OTA_TRIAL_MARK_VALID);
    in.uptime_s = settled - 1;
    CHECK(step(in) == OTA_TRIAL_WAIT); // the same window as with a display
    in.uptime_s = settled;
    in.c6_up    = false;
    CHECK(step(in) == OTA_TRIAL_STAY); // still needs the C6 link: without it nobody can replace the image
    in.uptime_s = OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
    CHECK(step(in) == OTA_TRIAL_ROLL_BACK);
    in           = up_at(settled);
    in.display   = false;
    in.boot_done = false;
    CHECK(step(in) != OTA_TRIAL_MARK_VALID);
}

// A live 120 MHz PSRAM trial defers the mark until it ends; the rollback for a dead C6 link still wins
static void test_psram_trial(void) {
    ota_trial_in_t in = up_at(OTA_VALID_AFTER_S + 10);
    in.psram_trial    = true;
    CHECK(step(in) == OTA_TRIAL_WAIT);
    in.display = false;
    CHECK(step(in) == OTA_TRIAL_WAIT);
    in.psram_trial = false;
    CHECK(step(in) == OTA_TRIAL_MARK_VALID);
    in             = up_at(OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S);
    in.psram_trial = true;
    in.c6_up       = false;
    CHECK(step(in) == OTA_TRIAL_ROLL_BACK);
}

// Nothing to decide for an image that isn't on trial, whatever else is true
static void test_not_on_trial(void) {
    ota_trial_in_t in = up_at(OTA_VALID_AFTER_S);
    in.pending        = false;
    CHECK(step(in) == OTA_TRIAL_DONE);
    in.c6_up    = false;
    in.uptime_s = 100000;
    CHECK(step(in) == OTA_TRIAL_DONE);
}

int main(void) {
    test_image_header();
    test_display_boot();
    test_headless_boot();
    test_psram_trial();
    test_not_on_trial();

    // Rollback (#36): no C6 link on a trial image restarts it, but only after the grace time
    uint32_t const give_up = OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
    CHECK(ota_should_roll_back_now(true, false, give_up));
    CHECK(ota_should_roll_back_now(true, false, give_up + 1000));
    CHECK(!ota_should_roll_back_now(true, false, give_up - 1));
    CHECK(!ota_should_roll_back_now(true, false, 0));
    CHECK(!ota_should_roll_back_now(true, true, give_up + 1000)); // C6 up: Wi-Fi works, fix it over the air
    CHECK(!ota_should_roll_back_now(false, false, give_up + 1000)); // not on trial

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_valid_test: all checks passed\n");
    return 0;
}
