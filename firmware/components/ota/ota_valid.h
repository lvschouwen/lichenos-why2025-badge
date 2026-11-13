#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure logic, no ESP headers: also built into the host tests.

// How long a new image must run with everything up before it counts as good: a build that crashes or hangs within
// this time still rolls back on the next reset
#define OTA_VALID_AFTER_S 45
// An image installed through the Wi-Fi receiver (#411) also has to rejoin Wi-Fi before it counts as good: an image that
// boots but cannot join would otherwise strand an unattended badge, reachable only over USB. If it has not rejoined by
// this much uptime, the trial restarts so the bootloader boots the previous image. Worst case from reset to the
// rollback restart: this bound, or the end of the boot steps if they take longer, plus one trial second.
#define OTA_VALID_NEEDS_WIFI_S 300

// What ota knows about the running image (ota_image_trial_state): UNKNOWN when the OTA data could not be read yet
typedef enum { OTA_IMAGE_UNKNOWN, OTA_IMAGE_ON_TRIAL, OTA_IMAGE_NOT_ON_TRIAL } ota_image_state_t;
// For a caller that may restart (the PSRAM speed switch, the C6 supervisor): an unread state might be a trial, and a
// restart then rolls the image back, so only NOT_ON_TRIAL is "not on trial"
bool ota_image_may_be_on_trial(ota_image_state_t state);
// Whether the receiver refuses a request that stops the apps (#302): only on a definite trial, the state esp_ota_begin
// refuses too. An unread state is let through: refusing it would lock out Wi-Fi updates on a badge with blank otadata
bool ota_update_refused_on_trial(bool stops_apps, ota_image_state_t state);

// The restart gate (#304): a restart the badge makes on its own while this image is on trial rolls it back, so it
// waits (system_restart, polled once a second) until the image is valid, the trial loop has ended any other way (a
// restart then rolls back whenever it comes), OTA_RESTART_GATE_MAX_S of uptime or a power off is asked (heat never
// waits). A restart a person asked for, or one meant to roll back, goes ahead at once. 15 s past the trial loop's
// last second (OTA_VALID_NEEDS_WIFI_S for an image that came over Wi-Fi, #411), so the loop has ended first.
#define OTA_RESTART_GATE_MAX_S (OTA_VALID_NEEDS_WIFI_S + 15)

typedef enum {
    RESTART_GATE_GO,           // not on trial: restart
    RESTART_GATE_WAIT,         // automatic, on trial: look again in a second
    RESTART_GATE_GO_ASKED,     // on trial, but a person asked (or the rollback is meant): goes back to the previous one
    RESTART_GATE_GO_SETTLED,   // on trial, and the trial loop ended without the valid mark: waiting gains nothing
    RESTART_GATE_GO_BOUND,     // on trial, waited up to OTA_RESTART_GATE_MAX_S of uptime
    RESTART_GATE_GO_POWER_OFF, // on trial, but a power off was asked: it never waits
} restart_gate_t;

// image: the running image's state (ota_image_may_be_on_trial: UNKNOWN counts as on trial); trial_settled: main's
// trial loop has ended (any end); uptime_s: since boot; power_off: system_power_off_requested was called
restart_gate_t restart_gate_decide(bool asked, ota_image_state_t image, bool trial_settled, uint32_t uptime_s,
                                   bool power_off);
// Why a restart on trial goes ahead, for the log ("" for GO and WAIT)
char const *restart_gate_name(restart_gate_t gate);

// The display as this boot left it (#293). Only a deliberate headless boot (`fw noscreen`) lets the OS alone prove an
// image: a panel that failed to start may be the new image's fault, so that boot never marks it valid and the next
// reset rolls it back (with the C6 link up it stays on trial and Wi-Fi can still replace it)
// FAILED is 0 on purpose: a zeroed input is the cautious one
typedef enum { OTA_DISPLAY_FAILED, OTA_DISPLAY_UP, OTA_DISPLAY_SKIPPED } ota_display_t;

// How the running image arrived (#411). OTHER (0) is a serial flash, a first install or anything not proven to be a
// Wi-Fi install: it keeps the rule as before, so an image meant for a place without Wi-Fi is never rolled back for it
typedef enum { OTA_ARRIVAL_OTHER, OTA_ARRIVAL_WIFI } ota_arrival_t;

// The arrival mark: written by the Wi-Fi receiver at a successful install only, naming the slot it made the boot slot,
// and taken (read and cleared) once on the next boot. It lives in LP RAM (ota_trial.c): no console command or file
// push reaches it, a power-off or the EN reset of a serial flash loses it (which is OTHER, the old rule), and the slot
// check makes a mark for an image the bootloader did not boot (a rollback, a failed later install) mean nothing.
// test_no_wifi: a developer-mode test arm (`fw trial nowifi`, compiled out of release) that only ever makes the trial
// stricter: it counts Wi-Fi as not rejoined, to prove the rollback without touching an access point.
typedef struct {
    uint32_t magic;
    uint32_t slot_addr;
    uint32_t check;
    uint32_t test_no_wifi;
} ota_arrival_mark_t;

typedef struct {
    ota_arrival_t arrival;
    bool          test_no_wifi;
} ota_arrival_taken_t;

// The receiver installed an image into the slot at slot_addr and made it the boot slot; test_no_wifi kept as armed
void ota_arrival_mark(ota_arrival_mark_t *m, uint32_t slot_addr);
// Arms (or disarms) the test flag for the next install; it is cleared by the next take, so it is one-shot
void ota_arrival_arm_test(ota_arrival_mark_t *m, bool no_wifi);
// Once per boot: WIFI only for an intact mark naming the running slot. Always clears the mark.
ota_arrival_taken_t ota_arrival_take(ota_arrival_mark_t *m, uint32_t running_addr);

// What the trial sees once a second. It runs only after every boot step has returned (main.c app_main), so "the boot
// is done" is no input: a step that fails reports its own failure and blocks nothing, and the signals the image is
// responsible for are the inputs below.
typedef struct {
    ota_image_state_t image;
    ota_display_t     display;
    bool              shell_up;    // the shell is up (read only with OTA_DISPLAY_UP)
    bool              c6_up;       // the C6 link is up: Wi-Fi works, so the image can be replaced over the air
    uint32_t          uptime_s;
    ota_arrival_t     arrival;     // how this image arrived (#411)
    bool              wifi_joined; // associated with an address: what the receiver needs to serve a request
    bool              wifi_off;    // the user's own switch (Wi-Fi off, airplane mode) has Wi-Fi off
} ota_trial_in_t;

typedef enum {
    OTA_TRIAL_DONE,       // not on trial: nothing to do
    OTA_TRIAL_UNKNOWN,    // the state could not be read: read again in a second
    OTA_TRIAL_WAIT,       // look again in a second
    OTA_TRIAL_MARK_VALID, // the image proved itself: cancel the rollback
    OTA_TRIAL_ROLL_BACK,  // restart now so the bootloader boots the previous image
    OTA_TRIAL_STAY,       // stop looking: the image stays on trial, a reset goes back to the previous one
    OTA_TRIAL_ROLL_BACK_NO_WIFI, // came over Wi-Fi, not back on it by OTA_VALID_NEEDS_WIFI_S: restart to roll back
} ota_trial_act_t;

// A new image runs on trial until it proves itself: after OTA_VALID_AFTER_S the C6 link is up, no PSRAM trial is live
// and, with a display, the shell is up (with the display skipped on purpose the OS alone decides, #293; a failed one
// never proves it). Wi-Fi is not required, so a badge without (working) Wi-Fi credentials doesn't roll back for ever
// (#61). While the C6 link is down the trial keeps watching and rolls back at OTA_VALID_AFTER_S +
// OTA_ROLLBACK_GRACE_S (#36, #295); a live PSRAM trial is waited for up to that limit too; otherwise, past
// OTA_VALID_AFTER_S and not proven, it stops looking (STAY).
// An image that arrived over Wi-Fi (#411) must also have rejoined Wi-Fi to be proven, and with the UI down it stays on
// trial only once it is reachable over Wi-Fi; without Wi-Fi by OTA_VALID_NEEDS_WIFI_S it rolls back (the C6 rule above
// still rolls back sooner). With Wi-Fi switched off by the user's own setting it cannot rejoin, and rolling back what
// the user chose to take offline would be wrong: the rule as before applies then (still the C6 link and the UI).
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
    OTA_TRIAL_END_NO_WIFI,      // came over Wi-Fi and never rejoined it: the caller restarts now (#411)
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
