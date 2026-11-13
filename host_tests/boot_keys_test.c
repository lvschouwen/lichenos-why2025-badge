// "fw boot keys" (#379): the parse, the one-shot flag, a bad magic ignored, an unknown combo refused, and each plan
// reaching what a hand on the keyboard reaches (the #66 combo arms, Down is the Down arrow)
#include "boot_keys.h"
#include "factory_plan.h"
#include "kbd_map.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void parse_names(void) {
    boot_keys_t k = BOOT_KEYS_COUNT;
    CHECK(boot_keys_parse("down", &k) && k == BOOT_KEYS_DOWN);
    CHECK(boot_keys_parse("esc+bksp", &k) && k == BOOT_KEYS_ESC_BKSP);
    CHECK(boot_keys_parse("none", &k) && k == BOOT_KEYS_NONE);
    k = BOOT_KEYS_DOWN;
    CHECK(!boot_keys_parse("up", &k) && k == BOOT_KEYS_DOWN); // unknown: refused, out untouched
    CHECK(!boot_keys_parse("esc", &k) && !boot_keys_parse("", &k) && !boot_keys_parse("Down", &k));
    CHECK(!boot_keys_parse(NULL, &k));
    CHECK(strcmp(boot_keys_name(BOOT_KEYS_ESC_BKSP), "esc+bksp") == 0);
    CHECK(strcmp(boot_keys_name((boot_keys_t)99), "none") == 0);
}

static void one_shot(void) {
    boot_keys_flag_t f = {0};
    boot_keys_arm(&f, BOOT_KEYS_ESC_BKSP);
    CHECK(boot_keys_peek(&f) == BOOT_KEYS_ESC_BKSP);
    CHECK(boot_keys_peek(&f) == BOOT_KEYS_ESC_BKSP); // peek leaves it
    CHECK(boot_keys_take(&f) == BOOT_KEYS_ESC_BKSP);
    CHECK(boot_keys_take(&f) == BOOT_KEYS_NONE); // the next boot holds nothing
    CHECK(f.magic == 0 && f.combo == 0 && f.check == 0);
    boot_keys_arm(&f, BOOT_KEYS_DOWN);
    boot_keys_arm(&f, BOOT_KEYS_NONE); // none clears
    CHECK(boot_keys_take(&f) == BOOT_KEYS_NONE);
    boot_keys_arm(&f, (boot_keys_t)7); // out of range: clears, never arms
    CHECK(f.magic == 0 && boot_keys_take(&f) == BOOT_KEYS_NONE);
}

static void bad_flag_ignored(void) {
    boot_keys_flag_t f;
    boot_keys_arm(&f, BOOT_KEYS_DOWN);
    f.magic ^= 1; // power-on noise
    CHECK(boot_keys_take(&f) == BOOT_KEYS_NONE);
    CHECK(f.magic == 0); // cleared all the same
    boot_keys_arm(&f, BOOT_KEYS_DOWN);
    f.combo = BOOT_KEYS_ESC_BKSP; // the check word no longer matches
    CHECK(boot_keys_take(&f) == BOOT_KEYS_NONE);
    f = (boot_keys_flag_t){.magic = BOOT_KEYS_MAGIC, .combo = 9, .check = ~(BOOT_KEYS_MAGIC ^ 9u)};
    CHECK(boot_keys_take(&f) == BOOT_KEYS_NONE); // consistent but unknown
    memset(&f, 0xff, sizeof(f));
    CHECK(boot_keys_take(&f) == BOOT_KEYS_NONE);
}

// The plan fed to the #66 combo the way keyboard_inject feeds it: both pressed at once, released after hold_ms. The
// question must arm and the release must not cancel it.
static void esc_bksp_arms_the_reset_question(void) {
    boot_keys_plan_t p = boot_keys_plan(BOOT_KEYS_ESC_BKSP);
    CHECK(p.count == 2 && p.hold_ms >= FACTORY_COMBO_HOLD_MS);
    factory_combo_t c;
    factory_combo_init(&c);
    uint32_t   t     = 5; // a few ms after factory_combo_start
    bool       armed = false;
    for (uint8_t i = 0; i < p.count; i++) {
        armed |= factory_combo_key(&c, p.codes[i], true, t) == FC_EV_ARM;
    }
    for (uint32_t ms = t; ms < t + p.hold_ms; ms += 100) { // the combo task's tick
        armed |= factory_combo_tick(&c, ms) == FC_EV_ARM;
    }
    for (uint8_t i = 0; i < p.count; i++) {
        fc_event_t e = factory_combo_key(&c, p.codes[i], false, t + p.hold_ms);
        armed |= e == FC_EV_ARM;
        CHECK(e != FC_EV_CANCEL);
    }
    CHECK(armed && c.state == FC_ARMED);
}

static void down_is_the_down_arrow(void) {
    boot_keys_plan_t p = boot_keys_plan(BOOT_KEYS_DOWN);
    CHECK(p.count == 1 && kbd_map_key(p.codes[0], false) == KBD_KEY_DOWN);
    CHECK(p.hold_ms >= 2000 && p.hold_ms < FACTORY_COMBO_WINDOW_MS);
    CHECK(boot_keys_plan(BOOT_KEYS_NONE).count == 0);
    CHECK(boot_keys_plan((boot_keys_t)42).count == 0);
}

int main(void) {
    parse_names();
    one_shot();
    bad_flag_ignored();
    esc_bksp_arms_the_reset_question();
    down_is_the_down_arrow();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("boot_keys_test: ok\n");
    return 0;
}
