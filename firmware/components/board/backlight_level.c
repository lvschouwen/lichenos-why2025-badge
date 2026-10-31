#include "backlight_level.h"

#include <stdio.h>

uint8_t backlight_display_level(unsigned value) {
    if (value < BACKLIGHT_DISPLAY_MIN) {
        return BACKLIGHT_DISPLAY_MIN;
    }
    return value > 100 ? 100 : (uint8_t)value;
}

uint8_t backlight_keyboard_level(unsigned value) {
    return value > 100 ? 100 : (uint8_t)value;
}

size_t backlight_cmd_format(char *buf, size_t size, unsigned display, unsigned keyboard) {
    int len = snprintf(buf, size, "\nBL %u %u\n", display, keyboard);
    return len > 0 && (size_t)len < size ? (size_t)len : 0;
}
