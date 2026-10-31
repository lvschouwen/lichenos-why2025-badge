#include "backlight_level.h"

uint8_t backlight_display_level(unsigned value) {
    if (value < BACKLIGHT_DISPLAY_MIN) {
        return BACKLIGHT_DISPLAY_MIN;
    }
    return value > 100 ? 100 : (uint8_t)value;
}

uint8_t backlight_keyboard_level(unsigned value) {
    return value > 100 ? 100 : (uint8_t)value;
}
