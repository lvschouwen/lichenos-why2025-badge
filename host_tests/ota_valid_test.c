// Host test: when a freshly installed lean firmware (#61) may cancel the rollback
#include "ota_valid.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return (ota_trial_in_t){.image       = OTA_IMAGE_ON_TRIAL,
                            .display     = OTA_DISPLAY_UP,
                            .shell_up    = true,
                            .c6_up       = true,
                            .uptime_s    = uptime_s,
                            .wifi_joined = true}; // #411: a trial image needs Wi-Fi too
}

static ota_trial_act_t step(ota_trial_in_t in) {
    return ota_trial_step(&in);
}

// A display boot: the rules as before #293 (the shell and the C6 link up, OTA_VALID_AFTER_S)
static void test_display_up(void) {
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
}

// #293: only a boot that skipped the display on purpose (fw noscreen) is proven by the OS alone
static void test_display_skipped(void) {
    ota_trial_in_t in = up_at(OTA_VALID_AFTER_S);
    in.display        = OTA_DISPLAY_SKIPPED;
    in.shell_up       = false;
    CHECK(step(in) == OTA_TRIAL_MARK_VALID);
    in.uptime_s = OTA_VALID_AFTER_S - 1;
    CHECK(step(in) == OTA_TRIAL_WAIT); // the same window as with a display
    in.uptime_s    = OTA_VALID_AFTER_S;
    in.c6_up       = false;
    in.wifi_joined = false;            // no C6, no Wi-Fi
    CHECK(step(in) == OTA_TRIAL_WAIT); // still needs the C6 link: without it nobody can replace the image
}

// A display that failed to start may be the new image's fault: never proven, whatever the rest says
static void test_display_failed(void) {
    ota_trial_in_t in = up_at(OTA_VALID_AFTER_S);
    in.display        = OTA_DISPLAY_FAILED;
    CHECK(step(in) == OTA_TRIAL_STAY);
    in.shell_up = false;
    CHECK(step(in) == OTA_TRIAL_STAY);
    in.uptime_s = 100000;
    CHECK(step(in) == OTA_TRIAL_STAY);
    in.c6_up = false;
    CHECK(step(in) == OTA_TRIAL_ROLL_BACK);
    ota_trial_in_t zero = {
        .image = OTA_IMAGE_ON_TRIAL, .shell_up = true, .c6_up = true, .uptime_s = 100, .wifi_joined = true};
    CHECK(step(zero) == OTA_TRIAL_STAY); // a zeroed display field is the failed one
}

// For callers that may restart, a state nobody could read counts as a trial (#285 review)
static void test_may_be_on_trial(void) {
    CHECK(ota_image_may_be_on_trial(OTA_IMAGE_ON_TRIAL));
    CHECK(ota_image_may_be_on_trial(OTA_IMAGE_UNKNOWN));
    CHECK(!ota_image_may_be_on_trial(OTA_IMAGE_NOT_ON_TRIAL));
}

// #302 review: an update that stops the apps is refused only on a definite trial, as esp_ota_begin does. An unreadable
// state (blank otadata after a USB first install) must not lock Wi-Fi updates out for good
static void test_update_refused(void) {
    CHECK(ota_update_refused_on_trial(true, OTA_IMAGE_ON_TRIAL));
    CHECK(!ota_update_refused_on_trial(true, OTA_IMAGE_UNKNOWN));
    CHECK(!ota_update_refused_on_trial(true, OTA_IMAGE_NOT_ON_TRIAL));
    CHECK(!ota_update_refused_on_trial(false, OTA_IMAGE_ON_TRIAL));
}

// #420: the on-air list is not switched on while another image could run without asking; a USB-flashed badge (blank
// OTA data reads UNKNOWN) and a proven image are not pending
static void test_pending(void) {
    CHECK(ota_pending_why(false, false, OTA_IMAGE_NOT_ON_TRIAL, false, true) == NULL);
    CHECK(ota_pending_why(false, false, OTA_IMAGE_UNKNOWN, false, true) == NULL);
    CHECK(ota_pending_why(false, false, OTA_IMAGE_NOT_ON_TRIAL, false, false) == NULL); // no C6, nothing staged
    CHECK(ota_pending_why(true, false, OTA_IMAGE_NOT_ON_TRIAL, false, true) != NULL);
    CHECK(ota_pending_why(false, true, OTA_IMAGE_NOT_ON_TRIAL, false, true) != NULL);
    CHECK(ota_pending_why(false, false, OTA_IMAGE_ON_TRIAL, false, true) != NULL);
    CHECK(ota_pending_why(false, false, OTA_IMAGE_NOT_ON_TRIAL, true, true) != NULL);
    CHECK(ota_pending_why(false, false, OTA_IMAGE_UNKNOWN, true, true) != NULL);
    CHECK(strstr(ota_pending_why(true, true, OTA_IMAGE_ON_TRIAL, true, true), "being written"));
    CHECK(strstr(ota_pending_why(false, true, OTA_IMAGE_ON_TRIAL, true, true), "installed"));
    CHECK(strstr(ota_pending_why(false, false, OTA_IMAGE_ON_TRIAL, true, true), "trial"));
    CHECK(strstr(ota_pending_why(false, false, OTA_IMAGE_NOT_ON_TRIAL, true, true), "staged"));
    CHECK(strstr(ota_pending_why(false, false, OTA_IMAGE_NOT_ON_TRIAL, true, false), "not attached"));
}

// #420, the Dekker pair of ota_hold.h under sequential consistency, with the hand-off. Server: 0 store busy, 1 load
// onair_on (held: clear busy, stop), 2 store its result (installed, or the C6 change), 3 clear busy. List: 0 store
// onair_on, then three loads in load_order (of BUSY, RESULT, OTHER), then ota_pending_why on what it saw. A violation is
// the server finishing its change while the list stays on with nothing pending. Every interleaving of 4 + 4 steps.
enum { LOAD_BUSY, LOAD_RESULT, LOAD_OTHER };
static int dekker_violations(bool c6_change, int const load_order[3]) {
    int violations = 0;
    for (unsigned order = 0; order < 256; order++) {
        if (__builtin_popcount(order) != 4) {
            continue; // bit i set: step i is the server's
        }
        bool busy = false, result = false, on = false, held = false, seen[3] = {false, false, false};
        int  s = 0, l = 0;
        for (int i = 0; i < 8; i++) {
            if (order & (1u << i)) {
                int step = s++;
                if (step == 0) {
                    busy = true;
                } else if (step == 1) {
                    held = on;
                    busy = held ? false : busy;
                } else if (!held) {
                    busy   = step == 3 ? false : busy;
                    result = step == 2 ? true : result;
                }
            } else if (l++ == 0) {
                on = true;
            } else {
                int what   = load_order[l - 2];
                seen[what] = what == LOAD_BUSY ? busy : what == LOAD_RESULT ? result : false;
            }
        }
        bool installed = !c6_change && seen[LOAD_RESULT], c6 = c6_change && seen[LOAD_RESULT];
        bool pending   = ota_pending_why(seen[LOAD_BUSY], installed, OTA_IMAGE_NOT_ON_TRIAL, c6, true) != NULL;
        violations += !held && !pending;
    }
    return violations;
}

static void test_dekker(void) {
    static int const real[3]  = {LOAD_BUSY, LOAD_RESULT, LOAD_OTHER}; // ota_boot_pending's order
    static int const wrong[3] = {LOAD_RESULT, LOAD_OTHER, LOAD_BUSY}; // busy last: misses a whole hand-off
    CHECK(dekker_violations(false, real) == 0); // the boot slot switch
    CHECK(dekker_violations(true, real) == 0);  // a C6 file change
    CHECK(dekker_violations(false, wrong) > 0); // the model can fail: the order matters
    CHECK(dekker_violations(true, wrong) > 0);
}

static void test_image_state(void) {
    ota_trial_in_t in = up_at(OTA_VALID_AFTER_S);
    in.image          = OTA_IMAGE_NOT_ON_TRIAL;
    CHECK(step(in) == OTA_TRIAL_DONE);
    in.c6_up    = false;
    in.uptime_s = 100000;
    CHECK(step(in) == OTA_TRIAL_DONE);
    in.image = OTA_IMAGE_UNKNOWN;
    CHECK(step(in) == OTA_TRIAL_UNKNOWN); // never valid, never a rollback on a state nobody could read
}

// ---- The loop as main runs it (#295): a fake badge, one second per wait, the loop's own stop rule ----

typedef struct {
    uint32_t      uptime_s;      // now; the boot steps took this long
    uint32_t      c6_from_s;     // the C6 link is up from here (UINT32_MAX: never)
    uint32_t      shell_from_s;  // the shell is up from here
    ota_display_t display;
    int           unknown_reads; // the first reads fail
    int           mark_fails;    // the first marks fail
    int           marks;         // marks tried
    ota_arrival_t arrival;       // #411
    uint32_t      wifi_from_s;   // joined from here (UINT32_MAX: never) ...
    uint32_t      wifi_to_s;     // ... until here (the access point went away)
    bool          wifi_off;      // the person's saved switch
    uint32_t      hold_from_s;   // a thermal hold of Wi-Fi from here ... (0 with hold_to_s 0: none)
    uint32_t      hold_to_s;     // ... to here
    int           warns;         // trial_warn calls
    uint32_t      warn_at_s;
    uint32_t      warn_left_s;
} fake_t;

static bool in_hold(fake_t const *f) {
    return f->uptime_s >= f->hold_from_s && f->uptime_s < f->hold_to_s;
}

static void fake_read(void *ctx, ota_trial_in_t *in) {
    fake_t *f       = ctx;
    in->image       = f->unknown_reads > 0 ? (f->unknown_reads--, OTA_IMAGE_UNKNOWN) : OTA_IMAGE_ON_TRIAL;
    in->display     = f->display;
    in->shell_up    = f->display == OTA_DISPLAY_UP && f->uptime_s >= f->shell_from_s;
    in->c6_up       = f->uptime_s >= f->c6_from_s;
    in->uptime_s    = f->uptime_s;
    in->arrival     = f->arrival;
    in->wifi_off    = f->wifi_off;
    in->wifi_joined = !f->wifi_off && in->c6_up && f->uptime_s >= f->wifi_from_s && f->uptime_s < f->wifi_to_s &&
                      !in_hold(f);
    in->wifi_held   = !f->wifi_off && in_hold(f);
}

static bool fake_mark(void *ctx) {
    fake_t *f = ctx;
    f->marks++;
    return f->mark_fails > 0 ? (f->mark_fails--, false) : true;
}

static void fake_wait(void *ctx) {
    fake_t *f = ctx;
    f->uptime_s++;
    if (f->uptime_s > 100000) {
        printf("FAIL: the trial loop never stops\n");
        exit(1);
    }
}

static void fake_warn(void *ctx, uint32_t left_s) {
    fake_t *f = ctx;
    f->warns++;
    f->warn_at_s   = f->uptime_s;
    f->warn_left_s = left_s;
}

static ota_trial_end_t run(fake_t *f) {
    ota_trial_io_t const io = {
        .read = fake_read, .mark_valid = fake_mark, .wait = fake_wait, .warn = fake_warn, .ctx = f};
    return ota_trial_run(&io);
}

static fake_t badge(void) {
    return (fake_t){.uptime_s = 20, .c6_from_s = 0, .shell_from_s = 0, .display = OTA_DISPLAY_UP, .wifi_from_s = 0,
                    .wifi_to_s = UINT32_MAX};
}

static void test_loop_timeline(void) {
    uint32_t const give_up = OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
    fake_t         f       = badge(); // healthy: valid at 45 s, as before
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S && f.marks == 1);

    f           = badge(); // #295: the C6 link down until 106 s is watched to the rollback at 105 s
    f.c6_from_s = 106;
    CHECK(run(&f) == OTA_TRIAL_END_ROLL_BACK && f.uptime_s == give_up);

    f           = badge(); // the C6 link up at 60 s: valid then
    f.c6_from_s = 60;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == 60);

    f              = badge(); // never proven, C6 up: stays on trial at the window
    f.shell_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_STAY && f.uptime_s == OTA_VALID_AFTER_S && f.marks == 0);

    f         = badge(); // fw noscreen: valid at 45 s without the shell
    f.display = OTA_DISPLAY_SKIPPED;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S);

    f         = badge(); // a failed display: never valid; stays on trial (C6 up) ...
    f.display = OTA_DISPLAY_FAILED;
    CHECK(run(&f) == OTA_TRIAL_END_STAY && f.marks == 0);
    f.uptime_s  = 20; // ... or rolls back (C6 down)
    f.c6_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_ROLL_BACK && f.uptime_s == give_up && f.marks == 0);

}

static void test_loop_retries(void) {
    fake_t f     = badge(); // a mark that fails twice is tried again a second later
    f.mark_fails = 2;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.marks == 3 && f.uptime_s == OTA_VALID_AFTER_S + 2);

    f            = badge(); // ... and given up after OTA_TRIAL_TRIES
    f.mark_fails = 1000;
    CHECK(run(&f) == OTA_TRIAL_END_MARK_FAILED && f.marks == OTA_TRIAL_TRIES);

    f               = badge(); // a state read that fails at first is read again
    f.unknown_reads = OTA_TRIAL_TRIES - 1;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.marks == 1);

    f               = badge(); // ... and given up after OTA_TRIAL_TRIES, without marking or rolling back
    f.unknown_reads = 1000;
    CHECK(run(&f) == OTA_TRIAL_END_NO_STATE && f.marks == 0 && f.uptime_s == 20 + OTA_TRIAL_TRIES - 1);
}

// ---- #411: a trial image is valid only back on Wi-Fi, unless a slot switch started it ----

#define VALID OTA_TRIAL_MARK_VALID
#define WAIT OTA_TRIAL_WAIT
#define STAY OTA_TRIAL_STAY
#define BACK OTA_TRIAL_ROLL_BACK
#define NOWIFI OTA_TRIAL_ROLL_BACK_NO_WIFI
#define AIR OTA_ARRIVAL_OVER_AIR
#define SWITCH OTA_ARRIVAL_SLOT_SWITCH

typedef enum { UI_UP, UI_SHELL_DOWN, UI_SKIPPED, UI_FAILED } ui_t;

typedef struct {
    ota_arrival_t   arrival;
    uint32_t        t, held_s;
    bool            c6, joined, off, held;
    ui_t            ui;
    ota_trial_act_t want;
    char const     *why;
} row_t;

static ota_trial_in_t row_in(row_t const *r) {
    static ota_display_t const disp[] = {OTA_DISPLAY_UP, OTA_DISPLAY_UP, OTA_DISPLAY_SKIPPED, OTA_DISPLAY_FAILED};
    return (ota_trial_in_t){.image       = OTA_IMAGE_ON_TRIAL,
                            .display     = disp[r->ui],
                            .shell_up    = r->ui == UI_UP,
                            .c6_up       = r->c6,
                            .uptime_s    = r->t,
                            .arrival     = r->arrival,
                            .wifi_joined = r->joined,
                            .wifi_off    = r->off,
                            .wifi_held   = r->held,
                            .held_s      = r->held_s};
}

// The cases that matter, written out (not derived from the code)
static void test_wifi_table(void) {
    uint32_t const v = OTA_VALID_AFTER_S, g = OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S, w = OTA_VALID_NEEDS_WIFI_S;
    row_t const rows[] = {
        // arrival t held_s c6 joined off held ui want
        {AIR, v, 0, 1, 1, 0, 0, UI_UP, VALID, "everything up and back on Wi-Fi"},
        {AIR, v - 1, 0, 1, 1, 0, 0, UI_UP, WAIT, "too soon, as before"},
        {AIR, v, 0, 1, 0, 0, 0, UI_UP, WAIT, "everything up but Wi-Fi: not proven"},
        {AIR, w - 1, 0, 1, 0, 0, 0, UI_UP, WAIT, "a second before the bound"},
        {AIR, w, 0, 1, 0, 0, 0, UI_UP, NOWIFI, "the bound without Wi-Fi"},
        {AIR, w, 0, 1, 0, 0, 0, UI_FAILED, NOWIFI, "a failed display without Wi-Fi rolls back too"},
        {AIR, w + 500, 0, 1, 1, 0, 0, UI_UP, VALID, "a late join still proves it"},
        {AIR, v, 0, 1, 1, 0, 0, UI_SHELL_DOWN, STAY, "UI down but reachable over Wi-Fi"},
        {AIR, v, 0, 1, 1, 0, 0, UI_FAILED, STAY, "display failed but reachable over Wi-Fi"},
        {AIR, v, 0, 1, 1, 0, 0, UI_SKIPPED, VALID, "fw noscreen, back on Wi-Fi"},
        {AIR, g, 0, 0, 0, 0, 0, UI_UP, BACK, "no C6: the old rollback, sooner"},
        {AIR, g - 1, 0, 0, 0, 0, 0, UI_UP, WAIT, "no C6, before its rollback"},
        {AIR, v, 0, 1, 0, 1, 0, UI_UP, VALID, "Wi-Fi switched off by the person: the old rule"},
        {AIR, w, 0, 1, 0, 1, 0, UI_SHELL_DOWN, STAY, "switched off, UI down: the old STAY, no rollback"},
        {AIR, g, 0, 0, 0, 1, 0, UI_UP, BACK, "switched off, no C6: the old rollback"},
        {AIR, v, 0, 1, 0, 0, 1, UI_UP, WAIT, "thermal hold: not proven by the old rule"},
        {AIR, w + 100, 0, 1, 0, 0, 1, UI_UP, WAIT, "thermal hold: never rolled back for Wi-Fi"},
        {AIR, w + 100, 101, 1, 0, 0, 0, UI_UP, WAIT, "the held seconds do not count"},
        {AIR, w + 100, 100, 1, 0, 0, 0, UI_UP, NOWIFI, "the bound outside the hold"},
        {AIR, g, 0, 0, 0, 0, 1, UI_UP, BACK, "a hold does not save an image without its C6"},
        {SWITCH, v, 0, 1, 0, 0, 0, UI_UP, VALID, "fw slot other: the old rule, no Wi-Fi needed"},
        {SWITCH, w, 0, 1, 0, 0, 0, UI_SHELL_DOWN, STAY, "fw slot other, UI down: the old STAY"},
        {SWITCH, g, 0, 0, 0, 0, 0, UI_UP, BACK, "fw slot other, no C6: the old rollback"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        ota_trial_in_t in = row_in(&rows[i]);
        if (ota_trial_step(&in) != rows[i].want) {
            printf("FAIL table row %zu (%s): got %d want %d\n", i, rows[i].why, ota_trial_step(&in), rows[i].want);
            failures++;
        }
    }
}

// Every combination: arrival x uptime x Wi-Fi joined x C6 x UI x Wi-Fi off x held, checked for the properties the
// issue states
static void test_wifi_sweep(void) {
    uint32_t const times[] = {0, OTA_VALID_AFTER_S - 1, OTA_VALID_AFTER_S, OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S,
                              OTA_NO_WIFI_WARN_S, OTA_VALID_NEEDS_WIFI_S - 1, OTA_VALID_NEEDS_WIFI_S, 100000};
    int            cases   = 0;
    for (int arr = 0; arr < 2; arr++) {
        for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
            for (int bits = 0; bits < 16; bits++) {
                for (int ui = UI_UP; ui <= UI_FAILED; ui++) {
                    row_t r = {.arrival = arr ? SWITCH : AIR,
                               .t       = times[t],
                               .c6      = bits & 1,
                               .joined  = (bits & 2) != 0,
                               .off     = (bits & 4) != 0,
                               .held    = (bits & 8) != 0,
                               .ui      = (ui_t)ui};
                    ota_trial_in_t  in   = row_in(&r);
                    ota_trial_act_t got  = ota_trial_step(&in);
                    bool            rule = r.arrival == AIR && !r.off;
                    if (!rule) { // the old rule: nothing about Wi-Fi changes the answer
                        ota_trial_in_t other = in;
                        other.wifi_joined    = !in.wifi_joined;
                        other.wifi_held      = !in.wifi_held;
                        CHECK(ota_trial_step(&other) == got);
                        CHECK(got != NOWIFI);
                    }
                    CHECK(got != VALID || !rule || r.joined); // valid under the rule only back on Wi-Fi
                    CHECK(got != NOWIFI || !r.held);          // a thermal hold never rolls back for Wi-Fi
                    CHECK(got != NOWIFI || !r.joined);
                    in.image = OTA_IMAGE_NOT_ON_TRIAL; // never touch a valid image, nor an unreadable state
                    CHECK(ota_trial_step(&in) == OTA_TRIAL_DONE);
                    in.image = OTA_IMAGE_UNKNOWN;
                    CHECK(ota_trial_step(&in) == OTA_TRIAL_UNKNOWN);
                    cases++;
                }
            }
        }
    }
    CHECK(cases == 2 * 8 * 16 * 4);
}

// trial_read's Wi-Fi mapping (main.c): the saved switch, the hold, the radio and the address
static void test_wifi_glue(void) {
    ota_trial_in_t  in   = {0};
    ota_wifi_seen_t seen = {.switch_on = true, .radio_on = true, .has_ip = true};
    ota_trial_wifi_in(&seen, &in);
    CHECK(in.wifi_joined && !in.wifi_off && !in.wifi_held);
    seen.has_ip = false; // associated is not enough without an address
    ota_trial_wifi_in(&seen, &in);
    CHECK(!in.wifi_joined && !in.wifi_off);
    seen = (ota_wifi_seen_t){.switch_on = true, .held = true}; // thermal hold: the radio is off, the switch is not
    ota_trial_wifi_in(&seen, &in);
    CHECK(!in.wifi_joined && !in.wifi_off && in.wifi_held);
    seen = (ota_wifi_seen_t){.switch_on = false}; // switched off (or airplane): off, and a hold on top changes nothing
    ota_trial_wifi_in(&seen, &in);
    CHECK(in.wifi_off && !in.wifi_held && !in.wifi_joined);
    seen.held = true;
    ota_trial_wifi_in(&seen, &in);
    CHECK(in.wifi_off && !in.wifi_held);
    seen = (ota_wifi_seen_t){.test_no_wifi = true, .switch_on = true, .radio_on = true, .has_ip = true};
    ota_trial_wifi_in(&seen, &in); // the dev test arm: never joined, whatever Wi-Fi does
    CHECK(!in.wifi_joined && !in.wifi_off);
}

static void test_wifi_loop(void) {
    fake_t f      = badge(); // back on Wi-Fi before the window: valid at 45 s, as before
    f.wifi_from_s = 30;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S && f.warns == 0);

    f             = badge(); // a slow join: valid when it lands
    f.wifi_from_s = 200;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == 200 && f.marks == 1);

    f             = badge(); // never back on Wi-Fi: warned a minute ahead, rolled back at the bound, never marked
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_NO_WIFI && f.uptime_s == OTA_VALID_NEEDS_WIFI_S && f.marks == 0);
    CHECK(f.warns == 1 && f.warn_at_s == OTA_NO_WIFI_WARN_S && f.warn_left_s == 60);

    f             = badge(); // the boot steps ran past the bound: the first look rolls back, no warning first
    f.uptime_s    = OTA_VALID_NEEDS_WIFI_S + 7;
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_NO_WIFI && f.uptime_s == OTA_VALID_NEEDS_WIFI_S + 7 && f.warns == 0);

    f          = badge(); // Wi-Fi switched off by the person: the rule as before, valid at 45 s
    f.wifi_off = true;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S);

    f             = badge(); // no C6 link (so no Wi-Fi): the old rollback, sooner than the Wi-Fi bound
    f.c6_from_s   = UINT32_MAX;
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_ROLL_BACK && f.uptime_s == OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S);

    f              = badge(); // UI down: stays on trial once reachable over Wi-Fi
    f.shell_from_s = UINT32_MAX;
    f.wifi_from_s  = 60;
    CHECK(run(&f) == OTA_TRIAL_END_STAY && f.uptime_s == 60 && f.marks == 0);

    f              = badge(); // UI late, Wi-Fi joins at 30 and drops at 40 (a hold from 40 on): not proven at 45,
    f.shell_from_s = 50;      // waits through the hold, then proven once Wi-Fi is back at 100
    f.wifi_from_s  = 30;
    f.hold_from_s  = 40;
    f.hold_to_s    = 100;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == 100);

    f              = badge(); // joined at 30, the access point gone at 40, the UI up at 50: not proven, and with no
    f.shell_from_s = 50;      // rejoin it rolls back at the bound
    f.wifi_from_s  = 30;
    f.wifi_to_s    = 40;
    CHECK(run(&f) == OTA_TRIAL_END_NO_WIFI && f.uptime_s == OTA_VALID_NEEDS_WIFI_S && f.marks == 0);

    f             = badge(); // a thermal hold from 100 to 400: the clock stops, so the rollback comes 300 s later
    f.wifi_from_s = UINT32_MAX;
    f.hold_from_s = 100;
    f.hold_to_s   = 400;
    CHECK(run(&f) == OTA_TRIAL_END_NO_WIFI && f.uptime_s == OTA_VALID_NEEDS_WIFI_S + 300 && f.warns == 1);
    CHECK(f.warn_at_s == OTA_NO_WIFI_WARN_S + 300);

    f             = badge(); // ... and a hold that ends in a join proves the image, with no rollback
    f.wifi_from_s = 450;
    f.hold_from_s = 100;
    f.hold_to_s   = 450;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == 450);

    f             = badge(); // `fw slot other` without Wi-Fi: valid at 45 s, exactly as before
    f.arrival     = OTA_ARRIVAL_SLOT_SWITCH;
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S && f.warns == 0);

    // An automatic restart waits until the longest trial without a hold has ended (#304)
    CHECK(OTA_RESTART_GATE_MAX_S > OTA_VALID_NEEDS_WIFI_S);
    CHECK(OTA_VALID_NEEDS_WIFI_S > OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S);
    CHECK(OTA_NO_WIFI_WARN_S > OTA_VALID_AFTER_S && OTA_NO_WIFI_WARN_S < OTA_VALID_NEEDS_WIFI_S);
}

// The arrival mark: only an intact mark naming the running slot is a slot switch, and only once
static void test_arrival_mark(void) {
    uint32_t const     slot_b = 0x420000, slot_a = 0x20000;
    ota_arrival_mark_t m      = {0};
    CHECK(ota_arrival_take(&m, slot_a).arrival == AIR); // nothing written (an OTA, a lost mark): the Wi-Fi rule

    ota_arrival_mark_switch(&m, slot_b); // fw slot other to B, B boots
    ota_arrival_taken_t t = ota_arrival_take(&m, slot_b);
    CHECK(t.arrival == SWITCH && !t.test_no_wifi);
    CHECK(ota_arrival_take(&m, slot_b).arrival == AIR); // consumed: a later boot is not that switch

    ota_arrival_mark_switch(&m, slot_b); // switched to B, but the bootloader booted A (B invalid)
    CHECK(ota_arrival_take(&m, slot_a).arrival == AIR);
    CHECK(ota_arrival_take(&m, slot_b).arrival == AIR); // and cleared there too

    ota_arrival_mark_switch(&m, slot_a); // two switches in a row: the last one names the slot
    ota_arrival_mark_switch(&m, slot_b);
    CHECK(ota_arrival_take(&m, slot_b).arrival == SWITCH);

    // Power-on noise or a damaged mark is no slot switch: the Wi-Fi rule, which fails safe
    ota_arrival_mark_switch(&m, slot_b);
    m.check ^= 1;
    CHECK(ota_arrival_take(&m, slot_b).arrival == AIR);
    ota_arrival_mark_switch(&m, slot_b);
    m.slot_addr = slot_a;
    CHECK(ota_arrival_take(&m, slot_a).arrival == AIR);
    ota_arrival_mark_t noise = {.magic = 0xFFFFFFFFu, .slot_addr = slot_b, .check = 0, .test_no_wifi = 0xFFFFFFFFu};
    t                        = ota_arrival_take(&noise, slot_b);
    CHECK(t.arrival == AIR && !t.test_no_wifi);

    // The dev test arm: taken at the next boot, whatever slot it is, and one-shot
    ota_arrival_arm_test(&m, true);
    t = ota_arrival_take(&m, slot_b);
    CHECK(t.arrival == AIR && t.test_no_wifi);
    CHECK(!ota_arrival_take(&m, slot_b).test_no_wifi);
    ota_arrival_arm_test(&m, true); // armed, then a slot switch: both ride on the same boot
    ota_arrival_mark_switch(&m, slot_a);
    t = ota_arrival_take(&m, slot_a);
    CHECK(t.arrival == SWITCH && t.test_no_wifi);
    ota_arrival_arm_test(&m, true); // disarmed before the boot
    ota_arrival_arm_test(&m, false);
    CHECK(!ota_arrival_take(&m, slot_b).test_no_wifi);
}

int main(void) {
    test_image_header();
    test_display_up();
    test_display_skipped();
    test_display_failed();
    test_image_state();
    test_may_be_on_trial();
    test_update_refused();
    test_pending();
    test_dekker();
    test_loop_timeline();
    test_loop_retries();
    test_wifi_table();
    test_wifi_sweep();
    test_wifi_glue();
    test_wifi_loop();
    test_arrival_mark();

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
