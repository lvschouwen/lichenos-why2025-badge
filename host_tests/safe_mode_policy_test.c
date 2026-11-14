// Crash attribution and safe mode (#232): the LP RAM record (magic + checksum before use), 2 crashes of one app in 3
// boots switch its background off, the PSRAM re-arm restart after a crash counts as part of that crash (never as a
// clean boot), re-enabling forgets, the degraded boot, and the list of apps whose background is off.
#include "safe_mode_policy.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// One boot after a panic of the task that ran for id (NULL: an OS task)
static safe_mode_boot_t panic_boot(safe_mode_rec_t *r, char const *id) {
    safe_mode_note_panic(r, id);
    return safe_mode_boot(r, true);
}

static safe_mode_rec_t fresh(void) {
    safe_mode_rec_t r;
    memset(&r, 0xA5, sizeof(r)); // power-on noise
    safe_mode_boot(&r, false);
    return r;
}

static void noise_is_not_trusted(void) {
    safe_mode_rec_t r;
    memset(&r, 0xA5, sizeof(r));
    CHECK(!safe_mode_valid(&r));
    CHECK(!safe_mode_degraded(&r));
    r.magic = SAFE_MODE_MAGIC; // the magic alone is not enough
    CHECK(!safe_mode_valid(&r));
    memset(&r, 0, sizeof(r)); // nor is zeroed memory
    CHECK(!safe_mode_valid(&r));
    safe_mode_boot_t b = safe_mode_boot(&r, false);
    CHECK(safe_mode_valid(&r) && b.counted && !b.crash && !b.degraded);
    // Noise that claims a panic of an app is reset first: the crash is the boot's own verdict, blamed on nobody
    memset(&r, 0x61, sizeof(r)); // "aaaa...": would read as an id if the record were trusted
    b = safe_mode_boot(&r, true);
    CHECK(b.crash && b.app[0] == '\0' && !b.limit_reached);
}

static void checksum_guards_the_window(void) {
    safe_mode_rec_t  r = fresh();
    safe_mode_boot_t b = panic_boot(&r, "timer");
    CHECK(b.app_crashes == 1);
    r.window[1] ^= 0x10; // one bit lost in LP RAM
    CHECK(!safe_mode_valid(&r));
    b = panic_boot(&r, "timer"); // the window is forgotten: this is the first crash again, nobody is penalised
    CHECK(strcmp(b.app, "timer") == 0 && b.app_crashes == 1 && !b.limit_reached);
    r.check ^= 1u;
    CHECK(!safe_mode_valid(&r));
}

static void two_in_three_switch_off(void) {
    safe_mode_rec_t  r = fresh();
    safe_mode_boot_t b = panic_boot(&r, "timer");
    CHECK(b.counted && b.crash && strcmp(b.app, "timer") == 0 && b.app_crashes == 1 && !b.limit_reached);
    CHECK(b.degraded);
    b = safe_mode_boot(&r, false); // a clean boot in between
    CHECK(b.counted && !b.crash && b.degraded);
    b = panic_boot(&r, "timer");
    CHECK(b.app_crashes == 2 && b.limit_reached);
}

static void spread_out_crashes_do_not(void) {
    safe_mode_rec_t r = fresh();
    panic_boot(&r, "weather");
    safe_mode_boot(&r, false);
    safe_mode_boot(&r, false);
    safe_mode_boot_t b = panic_boot(&r, "weather"); // the first one left the window
    CHECK(b.app_crashes == 1 && !b.limit_reached);
    b = panic_boot(&r, "timer"); // another app's crash is not this one's
    CHECK(b.app_crashes == 1 && !b.limit_reached);
}

// The bench sequence (fw-869e608d, PSRAM at 120 MHz): a panic, then ~20 s later the software restart "psram 120 re-arm
// after crash". That pair is ONE crash: the re-arm boot takes no place in the window, so it neither dilutes the count
// as a clean boot nor counts as a second crash.
static void rearm_pair_is_one_crash(void) {
    safe_mode_rec_t  r = fresh();
    safe_mode_boot_t b = panic_boot(&r, "timer"); // boot B: after the panic, at 80 MHz
    CHECK(b.counted && b.app_crashes == 1 && !b.limit_reached);
    uint32_t before[SAFE_MODE_WINDOW];
    memcpy(before, r.window, sizeof(before));
    safe_mode_note_rearm(&r, true); // B ends in the re-arm restart
    b = safe_mode_boot(&r, false);  // boot C: a software reset, not a crash
    CHECK(!b.counted && !b.crash && b.app[0] == '\0' && !b.limit_reached);
    CHECK(memcmp(before, r.window, sizeof(before)) == 0); // C left the window as it was
    CHECK(b.degraded);                                    // still the crash's tail
    b = safe_mode_boot(&r, false);                        // the mark was one shot: the next boot is a real clean one
    CHECK(b.counted && !b.crash);

    // A crash, re-arm, an OS crash, re-arm, the app again: 2 of the app's in the 3 counted boots. Counting the two
    // re-arm boots as clean would have pushed the first crash out (1 of 3) and left the loop running.
    r = fresh();
    panic_boot(&r, "timer");
    safe_mode_note_rearm(&r, true);
    safe_mode_boot(&r, false);
    panic_boot(&r, NULL);
    safe_mode_note_rearm(&r, true);
    safe_mode_boot(&r, false);
    b = panic_boot(&r, "timer");
    CHECK(b.app_crashes == 2 && b.limit_reached);
}

static void rearm_edge_cases(void) {
    safe_mode_rec_t r = fresh();
    panic_boot(&r, "timer");
    safe_mode_note_rearm(&r, true);
    safe_mode_note_rearm(&r, false); // the arm did not save: no restart after all
    safe_mode_boot_t b = safe_mode_boot(&r, false);
    CHECK(b.counted && !b.crash); // a later restart is a clean boot again

    r = fresh();
    panic_boot(&r, "timer");
    safe_mode_note_rearm(&r, true);
    b = panic_boot(&r, "timer"); // it crashed again before the re-arm restart came
    CHECK(b.counted && b.app_crashes == 2 && b.limit_reached);

    safe_mode_rec_t noise;
    memset(&noise, 0x5A, sizeof(noise));
    safe_mode_note_rearm(&noise, true); // on a record that is not valid: reset, then marked
    CHECK(safe_mode_valid(&noise) && (noise.flags & SAFE_MODE_F_REARM));
}

static void nobody_is_blamed(void) {
    safe_mode_rec_t  r = fresh();
    safe_mode_boot_t b = panic_boot(&r, NULL); // the console's task ("fw crash test panic"), an IDF task, an ISR
    CHECK(b.crash && b.app[0] == '\0' && b.app_crashes == 0 && !b.limit_reached && b.degraded);
    b = panic_boot(&r, "");
    CHECK(b.crash && b.app[0] == '\0' && !b.limit_reached);
    b = safe_mode_boot(&r, true); // a crash without a panic (brownout, lockup, a watchdog reset): nobody
    CHECK(b.crash && b.app[0] == '\0' && r.window[0] == SAFE_MODE_NOBODY);
    // Ids the OS would never register are nobody too
    char const *bad[] = {"Timer", "time-r", "a,b", "abcdefghijklmn" /* 14 */, " timer"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        b = panic_boot(&r, bad[i]);
        CHECK(b.crash && b.app[0] == '\0');
    }
    b = panic_boot(&r, "abcdefghijklm"); // 13: the longest id
    CHECK(strcmp(b.app, "abcdefghijklm") == 0);
}

static void panic_mark_alone_is_a_crash(void) {
    safe_mode_rec_t r = fresh();
    safe_mode_note_panic(&r, "timer");
    safe_mode_boot_t b = safe_mode_boot(&r, false); // the reset reason said otherwise, but the handler ran
    CHECK(b.crash && strcmp(b.app, "timer") == 0);
    b = safe_mode_boot(&r, false); // consumed
    CHECK(!b.crash && r.crashed[0] == '\0' && r.flags == 0);
}

static void forget_resets(void) {
    safe_mode_rec_t r = fresh();
    panic_boot(&r, "timer");
    safe_mode_boot_t b = panic_boot(&r, "timer");
    CHECK(b.limit_reached);
    safe_mode_forget(&r, "timer");
    CHECK(safe_mode_valid(&r) && !safe_mode_degraded(&r));
    b = panic_boot(&r, "timer"); // switched on again: one crash is not two
    CHECK(b.app_crashes == 1 && !b.limit_reached);
    safe_mode_forget(&r, "weather"); // another app's: nothing changes
    CHECK(safe_mode_degraded(&r));
}

static void degraded_lasts_the_window(void) {
    safe_mode_rec_t r = fresh();
    panic_boot(&r, NULL);
    CHECK(safe_mode_boot(&r, false).degraded);
    CHECK(safe_mode_boot(&r, false).degraded);
    CHECK(!safe_mode_boot(&r, false).degraded); // three clean boots since
    CHECK(!safe_mode_degraded(&r));
}

// Review of #232: only an app with a background part is switched off; Timer (none) crashing twice is only counted
static void switch_off_needs_a_background(void) {
    safe_mode_rec_t r = fresh();
    panic_boot(&r, "timer");
    safe_mode_boot_t b = panic_boot(&r, "timer");
    CHECK(b.limit_reached);
    CHECK(!safe_mode_switch_off(&b, "timer", false)); // nothing to switch off
    CHECK(safe_mode_switch_off(&b, "timer", true));
    CHECK(!safe_mode_switch_off(&b, "weather", true)); // not the app this boot blames
    b = panic_boot(&r, "weather");
    CHECK(!b.limit_reached && !safe_mode_switch_off(&b, "weather", true)); // one crash is not two
    b = panic_boot(&r, NULL);
    CHECK(!safe_mode_switch_off(&b, "", true)); // nobody is never switched off
}

// Owner decision (2026-11-14): MeshCore is exempt. Its crashes count and make the boot degraded, but 2 in 3 never put
// it on the list: main passes may_switch_off false for an app with safe_mode_exempt.
static void exempt_app_is_never_listed(void) {
    safe_mode_rec_t r = fresh();
    char            list[SAFE_MODE_LIST_MAX] = "";
    panic_boot(&r, "meshcore");
    safe_mode_boot_t b = panic_boot(&r, "meshcore");
    CHECK(b.limit_reached && b.app_crashes == 2 && b.degraded && strcmp(b.app, "meshcore") == 0);
    bool exempt = true, has_background = true;
    if (safe_mode_switch_off(&b, "meshcore", has_background && !exempt)) {
        safe_mode_list_add(list, sizeof(list), "meshcore");
    }
    CHECK(list[0] == '\0' && !safe_mode_list_has(list, "meshcore"));
    b = panic_boot(&r, "meshcore"); // a third: still never
    CHECK(b.app_crashes == 3 && !safe_mode_switch_off(&b, "meshcore", false));
}

static void hash_and_size(void) {
    CHECK(sizeof(safe_mode_rec_t) <= 64);
    CHECK(safe_mode_hash("timer") != safe_mode_hash("weather"));
    CHECK(safe_mode_hash("timer") > SAFE_MODE_NOBODY && safe_mode_hash("") > SAFE_MODE_NOBODY);
    CHECK(safe_mode_hash(NULL) > SAFE_MODE_NOBODY);
}

static void list_rules(void) {
    char l[SAFE_MODE_LIST_MAX] = "";
    CHECK(!safe_mode_list_has(l, "timer"));
    CHECK(safe_mode_list_add(l, sizeof(l), "timer") && strcmp(l, "timer") == 0);
    CHECK(safe_mode_list_add(l, sizeof(l), "weather") && strcmp(l, "timer,weather") == 0);
    CHECK(safe_mode_list_add(l, sizeof(l), "timer") && strcmp(l, "weather,timer") == 0); // once only
    CHECK(safe_mode_list_has(l, "timer") && safe_mode_list_has(l, "weather") && !safe_mode_list_has(l, "time"));
    CHECK(safe_mode_list_remove(l, sizeof(l), "weather") && strcmp(l, "timer") == 0);
    CHECK(!safe_mode_list_remove(l, sizeof(l), "weather"));
    CHECK(safe_mode_list_remove(l, sizeof(l), "timer") && l[0] == '\0');
    CHECK(!safe_mode_list_add(l, sizeof(l), "Bad") && !safe_mode_list_add(l, sizeof(l), "") &&
          !safe_mode_list_add(l, sizeof(l), NULL) && !safe_mode_list_add(l, sizeof(l), "a,b"));
    CHECK(!safe_mode_list_add(NULL, 10, "timer") && !safe_mode_list_add(l, 0, "timer") &&
          !safe_mode_list_add(l, SAFE_MODE_LIST_MAX + 1, "timer"));
    CHECK(!safe_mode_list_has(NULL, "timer") && !safe_mode_list_has("timer", NULL));

    // Read back from flash: malformed entries match nothing and are dropped by the next change
    strcpy(l, ",,timer,BAD,x y,,weather,timer,abcdefghijklmnopq,");
    CHECK(safe_mode_list_has(l, "timer") && safe_mode_list_has(l, "weather") && !safe_mode_list_has(l, "BAD"));
    CHECK(!safe_mode_list_has(l, "abcdefghijklm"));
    CHECK(safe_mode_list_add(l, sizeof(l), "calc") && strcmp(l, "timer,weather,calc") == 0);
    strcpy(l, "timer,,BAD");
    CHECK(safe_mode_list_remove(l, sizeof(l), "timer") && l[0] == '\0');

    // No room: refused, the list untouched
    char small[12] = "timer";
    CHECK(!safe_mode_list_add(small, sizeof(small), "weather") && strcmp(small, "timer") == 0);
    CHECK(safe_mode_list_add(small, sizeof(small), "calc") && strcmp(small, "timer,calc") == 0);
    // Not NUL-terminated within cap: read no further than cap
    char full[8];
    memset(full, 'a', sizeof(full));
    CHECK(!safe_mode_list_remove(full, sizeof(full), "timer"));
}

int main(void) {
    noise_is_not_trusted();
    checksum_guards_the_window();
    two_in_three_switch_off();
    spread_out_crashes_do_not();
    rearm_pair_is_one_crash();
    rearm_edge_cases();
    nobody_is_blamed();
    panic_mark_alone_is_a_crash();
    forget_resets();
    degraded_lasts_the_window();
    switch_off_needs_a_background();
    exempt_app_is_never_listed();
    hash_and_size();
    list_rules();
    if (fails) {
        printf("safe_mode_policy_test: %d failed\n", fails);
        return 1;
    }
    printf("safe_mode_policy_test: ok\n");
    return 0;
}
