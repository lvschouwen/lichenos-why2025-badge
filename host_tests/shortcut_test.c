// Host test: the shape keys as OS shortcuts (#79)
#include "screen_ids.h"
#include "shortcut.h"
#include "ui_keys.h"

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

static void test_mapping(void) {
    // TCA8418 codes 2-7: ■ ▲ ✕ ● ☁ ◆ (Lucas, 2026-10-31)
    CHECK(shortcut_of_code(0x02) == SHORTCUT_HOME);
    CHECK(shortcut_of_code(0x03) == SHORTCUT_MESSAGES);
    CHECK(shortcut_of_code(0x04) == SHORTCUT_SWITCH_APP);
    CHECK(shortcut_of_code(0x05) == SHORTCUT_SEARCH);
    CHECK(shortcut_of_code(0x06) == SHORTCUT_NOTIFICATIONS);
    CHECK(shortcut_of_code(0x07) == SHORTCUT_POWER);
    // Nothing else is a shortcut: Esc, Backspace, letters, out of range
    CHECK(shortcut_of_code(0x00) == SHORTCUT_NONE);
    CHECK(shortcut_of_code(0x01) == SHORTCUT_NONE);
    CHECK(shortcut_of_code(0x08) == SHORTCUT_NONE);
    CHECK(shortcut_of_code(0x20) == SHORTCUT_NONE);
    CHECK(shortcut_of_code(0x51) == SHORTCUT_NONE);
    CHECK(shortcut_of_code(0xff) == SHORTCUT_NONE);
    CHECK(strcmp(shortcut_name(SHORTCUT_POWER), "power") == 0);
    CHECK(strcmp(shortcut_name(SHORTCUT_NONE), "none") == 0);
}

static void test_gate(void) {
    shortcut_gate_t open = {0};
    CHECK(shortcut_decide(0x02, true, &open) == SHORTCUT_HOME);
    CHECK(shortcut_decide(0x02, false, &open) == SHORTCUT_NONE); // the release does nothing
    CHECK(shortcut_decide(0x20, true, &open) == SHORTCUT_NONE);
    shortcut_gate_t g;
    g = open, g.dialog_open = true;
    CHECK(shortcut_decide(0x07, true, &g) == SHORTCUT_NONE); // not while a confirm dialog is open
    g = open, g.test_open = true;
    CHECK(shortcut_decide(0x02, true, &g) == SHORTCUT_NONE);
    g = open, g.booting = true;
    CHECK(shortcut_decide(0x06, true, &g) == SHORTCUT_NONE);
    g = open, g.update_busy = true;
    CHECK(shortcut_decide(0x02, true, &g) == SHORTCUT_NONE);
    g = open, g.setup_running = true;
    CHECK(shortcut_decide(0x03, true, &g) == SHORTCUT_NONE);
}

static void test_switch(void) {
    app_switch_t s = APP_SWITCH_INIT;
    CHECK(app_switch_target(&s, -1) == -1); // nothing opened yet
    app_switch_opened(&s, 2);
    CHECK(app_switch_target(&s, 2) == -1); // in the only app: nowhere to go
    CHECK(app_switch_target(&s, -1) == 2); // from home: back into it
    app_switch_opened(&s, 5);
    CHECK(app_switch_target(&s, 5) == 2);
    app_switch_opened(&s, 2); // ✕ opened it: the next ✕ goes back again
    CHECK(app_switch_target(&s, 2) == 5);
    app_switch_opened(&s, 2); // the same app again changes nothing
    CHECK(s.cur == 2 && s.prev == 5);
    CHECK(app_switch_target(&s, -1) == 2);
    app_switch_opened(&s, -1);
    CHECK(s.cur == 2 && s.prev == 5);
}

static void test_top_app(void) {
    int home_only[] = {0};
    CHECK(shortcut_top_app(home_only, 1) == -1);
    int in_app[] = {0, SCREEN_APP_OWN + 3, 170, 142}; // home, the app in slot 3, two of its own screens
    CHECK(shortcut_top_app(in_app, 4) == 3);
    int nested[] = {0, SCREEN_APP_OWN + 1, 110, SCREEN_APP_OWN + 5}; // an app > its page > the app in slot 5
    CHECK(shortcut_top_app(nested, 4) == 5);
    int ends[] = {0, SCREEN_APP_OWN + SCREEN_APP_MAX}; // the last slot of the handle
    CHECK(shortcut_top_app(ends, 2) == SCREEN_APP_MAX);
    int past[] = {0, 9, SCREEN_APP_OWN, SCREEN_APP_OWN + SCREEN_APP_MAX + 1, SCREEN_APP_BASE + 2}; // none is an app's screen
    CHECK(shortcut_top_app(past, 5) == -1);
}

static void test_send_tokens(void) {
    uint32_t k[8];
    size_t   pos = 99;
    CHECK(ui_keys_parse("square triangle cross circle cloud diamond", k, 8, &pos) == 6);
    for (int i = 0; i < 6; i++) {
        CHECK(UI_KEY_IS_SHAPE(k[i]) && UI_KEY_SHAPE_CODE(k[i]) == 2 + i);
        CHECK(shortcut_of_code(UI_KEY_SHAPE_CODE(k[i])) == (shortcut_t)(SHORTCUT_HOME + i));
    }
    CHECK(ui_keys_parse("down wait", k, 8, &pos) == 2);
    CHECK(!UI_KEY_IS_SHAPE(k[0]) && !UI_KEY_IS_SHAPE(k[1])); // nor is wait (UINT32_MAX)
}

int main(void) {
    test_mapping();
    test_gate();
    test_switch();
    test_top_app();
    test_send_tokens();
    if (failures) {
        return 1;
    }
    printf("shortcut_test: ok\n");
    return 0;
}
