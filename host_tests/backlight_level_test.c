// Host tests for the backlight level clamp: the screen never goes fully dark (a saved 0 meant a black screen on
// every boot), the keyboard may
#include "backlight_level.h"

#include <stdio.h>

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

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All backlight_level tests passed\n");
    return 0;
}
