#include "haptics_setting.h"

void haptics_setting_load(fw_get_u8_fn get, bool *enabled, hp_strength_t *strength) {
    uint8_t on = 1, str = HP_HIGH;
    if (get) {
        on  = get(HAPTICS_KEY_ENABLED, 1);
        str = get(HAPTICS_KEY_STRENGTH, HP_HIGH);
    }
    *enabled  = on != 0;
    *strength = str < HP_STRENGTH_COUNT ? (hp_strength_t)str : HP_HIGH;
}

bool haptics_setting_save_enabled(fw_set_u8_fn set, bool on) {
    return set && set(HAPTICS_KEY_ENABLED, on ? 1 : 0);
}

bool haptics_setting_save_strength(fw_set_u8_fn set, hp_strength_t s) {
    return set && set(HAPTICS_KEY_STRENGTH, (uint8_t)s);
}
