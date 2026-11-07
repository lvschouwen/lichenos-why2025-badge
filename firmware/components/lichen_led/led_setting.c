#include "led_setting.h"

bool led_setting_load(fw_get_u8_fn get) {
    return !get || get(LED_KEY_ENABLED, 1) != 0;
}

bool led_setting_save(fw_set_u8_fn set, bool on) {
    return set && set(LED_KEY_ENABLED, on ? 1 : 0);
}
