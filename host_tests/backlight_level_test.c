// Host tests for the backlight level clamp: the screen never goes fully dark (a saved 0 meant a black screen on
// every boot), the keyboard may
#include "backlight_level.h"

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

int main(void) {
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
