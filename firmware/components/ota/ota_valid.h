#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure logic, no ESP headers: also built into the host tests.

// How long a new image must run with everything up before it counts as good: a build that crashes or hangs within
// this time still rolls back on the next reset
#define OTA_VALID_AFTER_S 45

// What the trial of a new image (pending_verify) sees once a second, after every boot step has run (main.c)
typedef struct {
    bool     pending;     // the running image is on trial (ota_image_trial_pending)
    bool     boot_done;   // every boot step has run: the OS services, the apps and the update server are started
    bool     display;     // this boot has a display: only then is the shell one of the trial's inputs (#293)
    bool     shell_up;    // the shell is up (read only when display is true)
    bool     c6_up;       // the C6 link is up: Wi-Fi works, so the image can be replaced over the air
    bool     psram_trial; // a 120 MHz PSRAM trial is still live (psram_speed_trial_pending): it ends within a minute
    uint32_t uptime_s;
} ota_trial_in_t;

typedef enum {
    OTA_TRIAL_DONE,       // not on trial: nothing to do
    OTA_TRIAL_WAIT,       // look again in a second
    OTA_TRIAL_MARK_VALID, // the image proved itself: cancel the rollback
    OTA_TRIAL_ROLL_BACK,  // restart now so the bootloader boots the previous image
    OTA_TRIAL_STAY,       // stop looking: the image stays on trial, a reset goes back to the previous one
} ota_trial_act_t;

// A new image runs on trial until it proves itself: the OS is up (every boot step ran, the C6 link is up, no PSRAM
// trial is live) and it has run for OTA_VALID_AFTER_S. With a display the shell must be up too; without one (a failed
// panel, `fw noscreen`) the OS alone decides, so a headless boot no longer rolls a good image back on the next reset
// (#293). Wi-Fi is not required, so a badge without (working) Wi-Fi credentials doesn't roll back for ever (#61).
// Past OTA_VALID_AFTER_S without that the trial stops looking (STAY), unless the C6 link has been down past the grace
// time (ROLL_BACK) or a PSRAM trial is still live (WAIT: it ends, passed or failed, within PSRAM_SPEED_TRIAL_SECONDS).
ota_trial_act_t ota_trial_step(ota_trial_in_t const *in);

// How long past OTA_VALID_AFTER_S a trial image may stay without its C6 link before it gives up
#define OTA_ROLLBACK_GRACE_S 60

// A trial image whose C6 link never came up has no Wi-Fi, so nobody can fix it over the air: restart so the bootloader
// boots the previous image (#36). With the C6 up (UI down only) Wi-Fi works and the image stays on trial.
bool ota_should_roll_back_now(bool pending_verify, bool c6_up, uint32_t uptime_s);

// The first bytes of an app image (esp_image_header_t, 24 bytes): checked before a slot is erased, so a wrong file or
// another chip's image never costs the fallback slot (#5)
#define OTA_IMAGE_HEADER_LEN 24
bool ota_image_header_ok(uint8_t const *head, size_t len, uint16_t chip_id);
