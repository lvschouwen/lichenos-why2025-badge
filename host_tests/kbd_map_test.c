// Host test: the badge keyboard's TCA8418 key codes as LVGL keys and characters (#61)
#include "kbd_map.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// LVGL's key values (lv_group.h), spelled out so the test stays free of ESP and LVGL headers
#define LV_KEY_UP        17
#define LV_KEY_DOWN      18
#define LV_KEY_RIGHT     19
#define LV_KEY_LEFT      20
#define LV_KEY_ENTER     10
#define LV_KEY_BACKSPACE 8

int main(void) {
    // Codes from BadgeVMS's table (firmware/components/board/tca8418_keymap.h): 0x20 A, 0x3b Return, 0x08 Backspace,
    // 0x33 Left, 0x34 Down, 0x35 Right, 0x37 Up, 0x41 Space
    CHECK(kbd_map_key(0x20, false) == 'a');
    CHECK(kbd_map_key(0x20, true) == 'A');
    CHECK(kbd_map_key(0x41, false) == ' ');
    CHECK(kbd_map_key(0x3b, false) == LV_KEY_ENTER);
    CHECK(kbd_map_key(0x08, false) == LV_KEY_BACKSPACE);
    CHECK(kbd_map_key(0x37, false) == LV_KEY_UP);
    CHECK(kbd_map_key(0x34, false) == LV_KEY_DOWN);
    CHECK(kbd_map_key(0x33, false) == LV_KEY_LEFT);
    CHECK(kbd_map_key(0x35, false) == LV_KEY_RIGHT);

    // Shift itself types nothing
    CHECK(kbd_map_key(0x29, false) == 0);
    CHECK(kbd_map_is_shift(0x29) && kbd_map_is_shift(0x38) && !kbd_map_is_shift(0x20));

    // Outside the table
    CHECK(kbd_map_key(0x00, false) == 0);
    CHECK(kbd_map_key(0x51, false) == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("kbd_map_test: all checks passed\n");
    return 0;
}
