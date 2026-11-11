// lichen_sys_display.h and lichen_sys_power.h (#226) over board's backlight driver, so the shell never links to it.
// The driver keeps its names for `fw bl`, the boot path and the C6 reapply in main.
#include "lichen_sys_display.h"

#include "lichen_sys_power.h"

#include "backlight.h"

_Static_assert(LICHEN_SYS_DISPLAY_MIN == BACKLIGHT_DISPLAY_MIN, "the gate's screen minimum follows board's");
_Static_assert(LICHEN_SYS_DISPLAY_MAX == BACKLIGHT_DISPLAY_MAX, "the gate's screen maximum follows board's");

void lichen_sys_display_set(uint8_t display, uint8_t keyboard) {
    backlight_set(display, keyboard);
}

void lichen_sys_display_set_saved(uint8_t display, uint8_t keyboard) {
    backlight_set_saved(display, keyboard);
}

void lichen_sys_display_get(uint8_t *display, uint8_t *keyboard) {
    backlight_get(display, keyboard);
}

void lichen_sys_display_idle(uint8_t display, uint8_t keyboard) {
    backlight_idle(display, keyboard);
}

void lichen_sys_display_resume(void) {
    backlight_resume();
}

void lichen_sys_display_temporary(uint8_t display, uint8_t keyboard) {
    backlight_temporary(display, keyboard);
}

void lichen_sys_display_temporary_keyboard(uint8_t keyboard) {
    backlight_temporary_keyboard(keyboard);
}

uint32_t lichen_sys_display_user_generation(void) {
    return backlight_user_generation();
}

void lichen_sys_power_screen_off(void) {
    backlight_power_off();
}
