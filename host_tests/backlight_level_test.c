// Host tests for the backlight level clamp: the screen never goes fully dark (a saved 0 meant a black screen on
// every boot), the keyboard may
#include "backlight_level.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// A fake settings store: two keys, "missing" until set
static int     stored[2] = {-1, -1};
static uint8_t fake_get(char const *key, uint8_t def) {
    int i = strcmp(key, BACKLIGHT_KEY_DISPLAY) == 0 ? 0 : strcmp(key, BACKLIGHT_KEY_KEYBOARD) == 0 ? 1 : -1;
    return i >= 0 && stored[i] >= 0 ? (uint8_t)stored[i] : def;
}
static bool fake_set(char const *key, uint8_t value) {
    int i = strcmp(key, BACKLIGHT_KEY_DISPLAY) == 0 ? 0 : strcmp(key, BACKLIGHT_KEY_KEYBOARD) == 0 ? 1 : -1;
    if (i < 0) {
        return false;
    }
    stored[i] = value;
    return true;
}

static void test_store(void) {
    // the keys are the badge_fw ones from before #224: renaming one would silently reset the saved levels
    CHECK(strcmp(BACKLIGHT_KEY_DISPLAY, "bl_disp") == 0);
    CHECK(strcmp(BACKLIGHT_KEY_KEYBOARD, "bl_kbd") == 0);
    CHECK(strcmp(BACKLIGHT_KEY_R16, "r16") == 0);
    uint8_t d = 1, k = 1;
    backlight_levels_load(NULL, &d, &k); // not bound yet
    CHECK(d == BACKLIGHT_DEFAULT_DISPLAY && k == BACKLIGHT_DEFAULT_KEYBOARD);
    backlight_levels_load(fake_get, &d, &k); // storage wiped: the defaults
    CHECK(d == 30 && k == 10);
    CHECK(!backlight_levels_save(NULL, 50, 20));
    CHECK(backlight_levels_save(fake_set, 60, 0)); // a keyboard level of 0 is a real level
    backlight_levels_load(fake_get, &d, &k);
    CHECK(d == 60 && k == 0);
    stored[0] = 0; // an older build saved a screen level of 0
    stored[1] = 250;
    backlight_levels_load(fake_get, &d, &k);
    CHECK(d == BACKLIGHT_DISPLAY_MIN && k == 100);
    backlight_levels_load(fake_get, NULL, NULL); // either out may be NULL
}

int main(void) {
    test_store();
    CHECK(BACKLIGHT_DISPLAY_MIN == 5);
    CHECK(backlight_display_level(0) == BACKLIGHT_DISPLAY_MIN);
    CHECK(backlight_display_level(4) == BACKLIGHT_DISPLAY_MIN);
    CHECK(backlight_display_level(5) == 5);
    CHECK(backlight_display_level(30) == 30);
    CHECK(backlight_display_level(100) == 100);
    CHECK(backlight_display_level(101) == 100);
    CHECK(backlight_display_level(255) == 100);

    CHECK(backlight_keyboard_level(0) == 0);
    CHECK(backlight_keyboard_level(10) == 10);
    CHECK(backlight_keyboard_level(100) == 100);
    CHECK(backlight_keyboard_level(200) == 100);

    char   cmd[24];
    size_t len = backlight_cmd_format(cmd, sizeof(cmd), 0, 0); // not clamped to the screen minimum
    CHECK(len == 8 && strcmp(cmd, "\nBL 0 0\n") == 0);
    len = backlight_cmd_format(cmd, sizeof(cmd), 30, 10);
    CHECK(len == 10 && strcmp(cmd, "\nBL 30 10\n") == 0);
    len = backlight_cmd_format(cmd, sizeof(cmd), 100, 100);
    CHECK(len == 12 && strcmp(cmd, "\nBL 100 100\n") == 0);
    CHECK(backlight_cmd_format(cmd, 8, 0, 0) == 0); // needs 8 bytes plus the NUL
    CHECK(backlight_cmd_format(cmd, 9, 0, 0) == 8);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All backlight_level tests passed\n");
    return 0;
}
