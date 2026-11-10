#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure logic, no ESP headers: also built into the host tests.

// How long a new image must run with everything up before it counts as good: a build that crashes or hangs within
// this time still rolls back on the next reset
#define OTA_VALID_AFTER_S 45

// What ota knows about the running image (ota_image_trial_state): UNKNOWN when the OTA data could not be read yet
typedef enum { OTA_IMAGE_UNKNOWN, OTA_IMAGE_ON_TRIAL, OTA_IMAGE_NOT_ON_TRIAL } ota_image_state_t;

// The display as this boot left it (#293). Only a deliberate headless boot (`fw noscreen`) lets the OS alone prove an
// image: a panel that failed to start may be the new image's fault, so that boot never marks it valid and the next
// reset rolls it back (with the C6 link up it stays on trial and Wi-Fi can still replace it)
// FAILED is 0 on purpose: a zeroed input is the cautious one
typedef enum { OTA_DISPLAY_FAILED, OTA_DISPLAY_UP, OTA_DISPLAY_SKIPPED } ota_display_t;

// What the trial sees once a second. It runs only after every boot step has returned (main.c app_main), so "the boot
// is done" is no input: a step that fails reports its own failure and blocks nothing, and the signals the image is
// responsible for are the inputs below.
typedef struct {
    ota_image_state_t image;
    ota_display_t     display;
    bool              shell_up;    // the shell is up (read only with OTA_DISPLAY_UP)
    bool              c6_up;       // the C6 link is up: Wi-Fi works, so the image can be replaced over the air
    bool              psram_trial; // a 120 MHz PSRAM trial is live (psram_speed_trial_pending): it ends within a minute
    uint32_t          uptime_s;
} ota_trial_in_t;

typedef enum {
    OTA_TRIAL_DONE,       // not on trial: nothing to do
    OTA_TRIAL_UNKNOWN,    // the state could not be read: read again in a second
    OTA_TRIAL_WAIT,       // look again in a second
    OTA_TRIAL_MARK_VALID, // the image proved itself: cancel the rollback
    OTA_TRIAL_ROLL_BACK,  // restart now so the bootloader boots the previous image
    OTA_TRIAL_STAY,       // stop looking: the image stays on trial, a reset goes back to the previous one
} ota_trial_act_t;

// A new image runs on trial until it proves itself: after OTA_VALID_AFTER_S the C6 link is up, no PSRAM trial is live
// and, with a display, the shell is up (with the display skipped on purpose the OS alone decides, #293; a failed one
// never proves it). Wi-Fi is not required, so a badge without (working) Wi-Fi credentials doesn't roll back for ever
// (#61). While the C6 link is down the trial keeps watching and rolls back at OTA_VALID_AFTER_S +
// OTA_ROLLBACK_GRACE_S (#36, #295); a live PSRAM trial is waited for too; otherwise, past OTA_VALID_AFTER_S and not
// proven, it stops looking (STAY).
ota_trial_act_t ota_trial_step(ota_trial_in_t const *in);

// The trial loop main runs (#295: its stop rule is host-tested with it): read, decide, act, one second, again. read
// fills every input; mark_valid cancels the rollback (false: not saved); wait sleeps one second.
typedef struct {
    void (*read)(void *ctx, ota_trial_in_t *in);
    bool (*mark_valid)(void *ctx);
    void (*wait)(void *ctx);
    void *ctx;
} ota_trial_io_t;

typedef enum {
    OTA_TRIAL_END_NOT_ON_TRIAL, // nothing to do
    OTA_TRIAL_END_VALID,        // marked valid
    OTA_TRIAL_END_ROLL_BACK,    // the caller restarts now
    OTA_TRIAL_END_STAY,         // not proven: stays on trial
    OTA_TRIAL_END_MARK_FAILED,  // proven, but the mark could not be saved OTA_TRIAL_TRIES times: stays on trial
    OTA_TRIAL_END_NO_STATE,     // the state could not be read OTA_TRIAL_TRIES times: if on trial, a reset rolls back
} ota_trial_end_t;

// Reads of the state and saves of the mark, one a second, before the loop gives up on them
#define OTA_TRIAL_TRIES 5

ota_trial_end_t ota_trial_run(ota_trial_io_t const *io);

// How long past OTA_VALID_AFTER_S a trial image may stay without its C6 link before it gives up
#define OTA_ROLLBACK_GRACE_S 60

// A trial image whose C6 link never came up has no Wi-Fi, so nobody can fix it over the air: restart so the bootloader
// boots the previous image (#36). With the C6 up (UI down only) Wi-Fi works and the image stays on trial.
bool ota_should_roll_back_now(bool pending_verify, bool c6_up, uint32_t uptime_s);

// The first bytes of an app image (esp_image_header_t, 24 bytes): checked before a slot is erased, so a wrong file or
// another chip's image never costs the fallback slot (#5)
#define OTA_IMAGE_HEADER_LEN 24
bool ota_image_header_ok(uint8_t const *head, size_t len, uint16_t chip_id);
