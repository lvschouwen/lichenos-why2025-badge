// Host test: when a freshly installed lean firmware (#61) may cancel the rollback
#include "ota_valid.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
    return (ota_trial_in_t){.image = OTA_IMAGE_ON_TRIAL, .display = OTA_DISPLAY_UP, .shell_up = true, .c6_up = true,
                            .uptime_s = uptime_s};
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
    in.uptime_s = OTA_VALID_AFTER_S;
    in.c6_up    = false;
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
    ota_trial_in_t zero = {.image = OTA_IMAGE_ON_TRIAL, .shell_up = true, .c6_up = true, .uptime_s = 100};
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
    uint32_t      wifi_from_s;   // joined from here (UINT32_MAX: never)
    bool          wifi_off;      // the user's switch
} fake_t;

static void fake_read(void *ctx, ota_trial_in_t *in) {
    fake_t *f       = ctx;
    in->image       = f->unknown_reads > 0 ? (f->unknown_reads--, OTA_IMAGE_UNKNOWN) : OTA_IMAGE_ON_TRIAL;
    in->display     = f->display;
    in->shell_up    = f->display == OTA_DISPLAY_UP && f->uptime_s >= f->shell_from_s;
    in->c6_up       = f->uptime_s >= f->c6_from_s;
    in->uptime_s    = f->uptime_s;
    in->arrival     = f->arrival;
    in->wifi_off    = f->wifi_off;
    in->wifi_joined = !f->wifi_off && f->uptime_s >= f->wifi_from_s;
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

static ota_trial_end_t run(fake_t *f) {
    ota_trial_io_t const io = {.read = fake_read, .mark_valid = fake_mark, .wait = fake_wait, .ctx = f};
    return ota_trial_run(&io);
}

static fake_t badge(void) {
    return (fake_t){.uptime_s = 20, .c6_from_s = 0, .shell_from_s = 0, .display = OTA_DISPLAY_UP, .wifi_from_s = 0};
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

// ---- #411: an image installed over Wi-Fi is valid only back on Wi-Fi ----

// The rule before #411, written as the spec states it: serial flashes and Wi-Fi off keep exactly this
static ota_trial_act_t legacy_expect(ota_trial_in_t const *in) {
    bool ui = in->display == OTA_DISPLAY_SKIPPED || (in->display == OTA_DISPLAY_UP && in->shell_up);
    if (in->uptime_s >= OTA_VALID_AFTER_S && in->c6_up && ui) {
        return OTA_TRIAL_MARK_VALID;
    }
    if (!in->c6_up) {
        return in->uptime_s >= OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S ? OTA_TRIAL_ROLL_BACK : OTA_TRIAL_WAIT;
    }
    return in->uptime_s < OTA_VALID_AFTER_S ? OTA_TRIAL_WAIT : OTA_TRIAL_STAY;
}

// The Wi-Fi rule as #411 states it
static ota_trial_act_t wifi_expect(ota_trial_in_t const *in) {
    bool ui = in->display == OTA_DISPLAY_SKIPPED || (in->display == OTA_DISPLAY_UP && in->shell_up);
    if (in->uptime_s >= OTA_VALID_AFTER_S && in->c6_up && ui && in->wifi_joined) {
        return OTA_TRIAL_MARK_VALID;
    }
    if (!in->c6_up && in->uptime_s >= OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S) {
        return OTA_TRIAL_ROLL_BACK;
    }
    if (in->wifi_joined && in->uptime_s >= OTA_VALID_AFTER_S) {
        return OTA_TRIAL_STAY; // reachable over Wi-Fi: the old reason to stay
    }
    return in->uptime_s >= OTA_VALID_NEEDS_WIFI_S ? OTA_TRIAL_ROLL_BACK_NO_WIFI : OTA_TRIAL_WAIT;
}

// Every combination: arrival x uptime x Wi-Fi joined x C6 x UI x Wi-Fi off by the user's setting
static void test_wifi_matrix(void) {
    uint32_t const times[] = {0,
                              OTA_VALID_AFTER_S - 1,
                              OTA_VALID_AFTER_S,
                              OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S - 1,
                              OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S,
                              OTA_VALID_NEEDS_WIFI_S - 1,
                              OTA_VALID_NEEDS_WIFI_S,
                              100000};
    struct {
        ota_display_t display;
        bool          shell_up;
    } const uis[] = {{OTA_DISPLAY_UP, true}, {OTA_DISPLAY_UP, false}, {OTA_DISPLAY_SKIPPED, false},
                     {OTA_DISPLAY_FAILED, true}};
    int cases = 0;
    for (int arr = 0; arr < 2; arr++) {
        for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
            for (int bits = 0; bits < 8; bits++) {
                for (size_t u = 0; u < sizeof(uis) / sizeof(uis[0]); u++) {
                    ota_trial_in_t in = {.image       = OTA_IMAGE_ON_TRIAL,
                                         .display     = uis[u].display,
                                         .shell_up    = uis[u].shell_up,
                                         .c6_up       = bits & 1,
                                         .uptime_s    = times[t],
                                         .arrival     = arr ? OTA_ARRIVAL_WIFI : OTA_ARRIVAL_OTHER,
                                         .wifi_joined = (bits & 2) != 0,
                                         .wifi_off    = (bits & 4) != 0};
                    bool            wifi_rule = in.arrival == OTA_ARRIVAL_WIFI && !in.wifi_off;
                    ota_trial_act_t want      = wifi_rule ? wifi_expect(&in) : legacy_expect(&in);
                    ota_trial_act_t got       = ota_trial_step(&in);
                    if (got != want) {
                        printf("FAIL matrix: arrival %d t %u c6 %d joined %d off %d ui %zu: got %d want %d\n", arr,
                               (unsigned)in.uptime_s, in.c6_up, in.wifi_joined, in.wifi_off, u, got, want);
                        failures++;
                    }
                    // Never a rollback for Wi-Fi on an image that did not come over Wi-Fi, or with Wi-Fi switched off
                    CHECK(wifi_rule || got != OTA_TRIAL_ROLL_BACK_NO_WIFI);
                    // Never a rollback of an image already valid, nor on a state nobody could read
                    in.image = OTA_IMAGE_NOT_ON_TRIAL;
                    CHECK(ota_trial_step(&in) == OTA_TRIAL_DONE);
                    in.image = OTA_IMAGE_UNKNOWN;
                    CHECK(ota_trial_step(&in) == OTA_TRIAL_UNKNOWN);
                    cases++;
                }
            }
        }
    }
    CHECK(cases == 2 * 8 * 8 * 4);
    // The headline cases, spelled out: everything up but Wi-Fi is not enough for a Wi-Fi install ...
    ota_trial_in_t in = up_at(OTA_VALID_AFTER_S);
    in.arrival        = OTA_ARRIVAL_WIFI;
    CHECK(step(in) == OTA_TRIAL_WAIT);
    in.uptime_s = OTA_VALID_NEEDS_WIFI_S;
    CHECK(step(in) == OTA_TRIAL_ROLL_BACK_NO_WIFI);
    in.wifi_joined = true; // ... back on Wi-Fi it is
    CHECK(step(in) == OTA_TRIAL_MARK_VALID);
    in.wifi_joined = false; // ... and with Wi-Fi switched off by the user, the rule as before
    in.wifi_off    = true;
    CHECK(step(in) == OTA_TRIAL_MARK_VALID);
    in.arrival  = OTA_ARRIVAL_OTHER; // a serial flash never needs Wi-Fi
    in.wifi_off = false;
    CHECK(step(in) == OTA_TRIAL_MARK_VALID);
}

static void test_wifi_loop(void) {
    fake_t f      = badge(); // back on Wi-Fi before the window: valid at 45 s, as before
    f.arrival     = OTA_ARRIVAL_WIFI;
    f.wifi_from_s = 30;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S);

    f             = badge(); // a slow join: valid when it lands
    f.arrival     = OTA_ARRIVAL_WIFI;
    f.wifi_from_s = 200;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == 200 && f.marks == 1);

    f             = badge(); // never back on Wi-Fi: rolls back at the bound, never marked
    f.arrival     = OTA_ARRIVAL_WIFI;
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_NO_WIFI && f.uptime_s == OTA_VALID_NEEDS_WIFI_S && f.marks == 0);

    f             = badge(); // the boot steps ran past the bound: the first look rolls back
    f.uptime_s    = OTA_VALID_NEEDS_WIFI_S + 7;
    f.arrival     = OTA_ARRIVAL_WIFI;
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_NO_WIFI && f.uptime_s == OTA_VALID_NEEDS_WIFI_S + 7);

    f          = badge(); // Wi-Fi switched off by the user: the rule as before, valid at 45 s
    f.arrival  = OTA_ARRIVAL_WIFI;
    f.wifi_off = true;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S);

    f             = badge(); // no C6 link (so no Wi-Fi): the old rollback, sooner than the Wi-Fi bound
    f.arrival     = OTA_ARRIVAL_WIFI;
    f.c6_from_s   = UINT32_MAX;
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_ROLL_BACK && f.uptime_s == OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S);

    f              = badge(); // UI down: stays on trial once reachable over Wi-Fi
    f.arrival      = OTA_ARRIVAL_WIFI;
    f.shell_from_s = UINT32_MAX;
    f.wifi_from_s  = 60;
    CHECK(run(&f) == OTA_TRIAL_END_STAY && f.uptime_s == 60 && f.marks == 0);

    f             = badge(); // a serial flash without Wi-Fi: valid at 45 s, exactly as before
    f.wifi_from_s = UINT32_MAX;
    CHECK(run(&f) == OTA_TRIAL_END_VALID && f.uptime_s == OTA_VALID_AFTER_S);

    // An automatic restart waits until the longest trial has ended (#304)
    CHECK(OTA_RESTART_GATE_MAX_S > OTA_VALID_NEEDS_WIFI_S);
    CHECK(OTA_VALID_NEEDS_WIFI_S > OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S);
}

// The arrival mark: only an intact mark naming the running slot counts, and only once
static void test_arrival_mark(void) {
    uint32_t const     slot_b = 0x220000, slot_a = 0x20000;
    ota_arrival_mark_t m      = {0};
    CHECK(ota_arrival_take(&m, slot_a).arrival == OTA_ARRIVAL_OTHER); // nothing written: a serial flash, first boot

    ota_arrival_mark(&m, slot_b); // installed into B, B boots
    ota_arrival_taken_t t = ota_arrival_take(&m, slot_b);
    CHECK(t.arrival == OTA_ARRIVAL_WIFI && !t.test_no_wifi);
    CHECK(ota_arrival_take(&m, slot_b).arrival == OTA_ARRIVAL_OTHER); // consumed: a later reboot is no new arrival

    ota_arrival_mark(&m, slot_b); // installed into B, but the bootloader booted A (B invalid, a rollback)
    CHECK(ota_arrival_take(&m, slot_a).arrival == OTA_ARRIVAL_OTHER);
    CHECK(ota_arrival_take(&m, slot_b).arrival == OTA_ARRIVAL_OTHER); // and cleared there too

    ota_arrival_mark(&m, slot_a); // two installs in a row: the last one names the slot
    ota_arrival_mark(&m, slot_b);
    CHECK(ota_arrival_take(&m, slot_b).arrival == OTA_ARRIVAL_WIFI);

    // Power-on noise: a mark whose check does not hold is no Wi-Fi install
    ota_arrival_mark(&m, slot_b);
    m.check ^= 1;
    CHECK(ota_arrival_take(&m, slot_b).arrival == OTA_ARRIVAL_OTHER);
    ota_arrival_mark(&m, slot_b);
    m.slot_addr = slot_a;
    CHECK(ota_arrival_take(&m, slot_a).arrival == OTA_ARRIVAL_OTHER);
    ota_arrival_mark_t noise = {.magic = 0xFFFFFFFFu, .slot_addr = slot_b, .check = 0, .test_no_wifi = 0xFFFFFFFFu};
    t = ota_arrival_take(&noise, slot_b);
    CHECK(t.arrival == OTA_ARRIVAL_OTHER && !t.test_no_wifi);

    // The dev test arm rides on the next install and is one-shot
    ota_arrival_arm_test(&m, true);
    ota_arrival_mark(&m, slot_b);
    t = ota_arrival_take(&m, slot_b);
    CHECK(t.arrival == OTA_ARRIVAL_WIFI && t.test_no_wifi);
    ota_arrival_mark(&m, slot_b);
    CHECK(!ota_arrival_take(&m, slot_b).test_no_wifi);
    ota_arrival_arm_test(&m, true); // armed, then no install: the next boot clears it
    CHECK(ota_arrival_take(&m, slot_a).arrival == OTA_ARRIVAL_OTHER);
    ota_arrival_mark(&m, slot_b);
    CHECK(!ota_arrival_take(&m, slot_b).test_no_wifi);
    ota_arrival_arm_test(&m, true); // disarmed before the install
    ota_arrival_arm_test(&m, false);
    ota_arrival_mark(&m, slot_b);
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
    test_loop_timeline();
    test_loop_retries();
    test_wifi_matrix();
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
