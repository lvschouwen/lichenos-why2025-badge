#include "backlight_level.h"

#include <stdio.h>

uint8_t backlight_display_level(unsigned value) {
    if (value < BACKLIGHT_DISPLAY_MIN) {
        return BACKLIGHT_DISPLAY_MIN;
    }
    return value > BACKLIGHT_DISPLAY_MAX ? BACKLIGHT_DISPLAY_MAX : (uint8_t)value;
}

uint8_t backlight_keyboard_level(unsigned value) {
    if (value == 0) {
        return 0;
    }
    if (value < BACKLIGHT_KEYBOARD_MIN) {
        return BACKLIGHT_KEYBOARD_MIN;
    }
    return value > BACKLIGHT_KEYBOARD_MAX ? BACKLIGHT_KEYBOARD_MAX : (uint8_t)value;
}

size_t backlight_cmd_format(char *buf, size_t size, unsigned display, unsigned keyboard) {
    int len = snprintf(buf, size, "\nBL %u %u\n", display, keyboard);
    return len > 0 && (size_t)len < size ? (size_t)len : 0;
}

void backlight_levels_load(backlight_get_u8_fn get, uint8_t *display, uint8_t *keyboard) {
    unsigned d = BACKLIGHT_DEFAULT_DISPLAY, k = BACKLIGHT_DEFAULT_KEYBOARD;
    if (get) {
        d = get(BACKLIGHT_KEY_DISPLAY, BACKLIGHT_DEFAULT_DISPLAY);
        k = get(BACKLIGHT_KEY_KEYBOARD, BACKLIGHT_DEFAULT_KEYBOARD);
    }
    if (display) {
        *display = backlight_display_level(d);
    }
    if (keyboard) {
        *keyboard = backlight_keyboard_level(k);
    }
}

bool backlight_levels_save(backlight_set_u8_fn set, uint8_t display, uint8_t keyboard) {
    if (!set) {
        return false;
    }
    bool ok = set(BACKLIGHT_KEY_DISPLAY, display);
    return set(BACKLIGHT_KEY_KEYBOARD, keyboard) && ok;
}
