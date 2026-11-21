// The owners' side of the registry's domain 0x16 (#501): each adapter calls the function the owner's own console
// command and Settings screen call, so the registry adds no second way to a setting. See switch_reg.h.
#include "switch_reg.h"

#include "backlight.h"
#include "haptics.h"
#include "led.h"
#include "lichen_radio.h"
#include "ota_autooff.h"
#include "ota_server.h"
#include "reg_ids.h"
#include "sensors.h"
#include "settings_backup.h"
#include "settings_store.h"

#include <stdatomic.h>

_Static_assert(RADIO_WIFI == 0 && RADIO_LORA == 1 && RADIO_BT == 2, "the rows' radio indexes follow radio_id_t");

_Static_assert(OTA_AUTOOFF_MAX_MIN == 240, "ota.autooff row range (reg_p4_switch.c)");

static _Atomic(void (*)(void)) ui_changed; // the shell's hook, bound once by main

void switch_reg_bind_ui(void (*changed)(void)) {
    atomic_store(&ui_changed, changed);
}

bool sw_radio_get(unsigned radio) {
    return radio < RADIO_COUNT && lichen_radio_get((radio_id_t)radio);
}

bool sw_radio_put(unsigned radio, bool on) {
    return radio < RADIO_COUNT && lichen_radio_set((radio_id_t)radio, on);
}

bool sw_airplane_get(void) {
    return lichen_airplane_get();
}

bool sw_airplane_put(bool on) {
    return lichen_airplane_set(on);
}

bool sw_haptics_get(void) {
    return haptics_enabled();
}

bool sw_haptics_put(bool on) {
    haptics_set_enabled(on);
    return true; // saved by the haptics service's own store hook
}

uint8_t sw_haptics_strength_get(void) {
    return (uint8_t)haptics_strength();
}

bool sw_haptics_strength_put(uint8_t strength) {
    haptics_set_strength((hp_strength_t)strength);
    return true;
}

uint8_t sw_led_mode_get(void) {
    return (uint8_t)led_override();
}

void sw_led_mode_put(uint8_t mode) {
    led_set_override((led_override_t)mode);
}

bool sw_ota_get(void) {
    return ota_server_enabled();
}

void sw_ota_put(bool on) {
    ota_server_set_enabled(on);
}

uint8_t sw_ota_autooff_get(void) {
    return ota_server_auto_off();
}

void sw_ota_autooff_put(uint8_t minutes) {
    ota_server_set_auto_off(minutes);
}

static sb_key_t const *cfg_key(char const *key) {
    return sb_find("badge_fw", key);
}

bool sw_cfg_get(char const *key, uint32_t *out) {
    sb_key_t const *k = cfg_key(key);
    if (!k || k->type != SB_U8) {
        return false;
    }
    *out = settings_get_u8(key, (uint8_t)k->def);
    return true;
}

bool sw_cfg_put(char const *key, uint32_t value) {
    sb_key_t const *k = cfg_key(key);
    if (!k || k->type != SB_U8 || value < k->min || value > k->max || !settings_set_u8(key, (uint8_t)value)) {
        return false;
    }
    void (*changed)(void) = atomic_load(&ui_changed);
    if (changed) {
        changed(); // the shell re-reads its policy (idle times, saver style, lock)
    }
    return true;
}

// NVS has no "is it stored": a stored value answers the same to two different defaults
static bool cfg_stored(char const *key, uint32_t *out) {
    uint8_t a = settings_get_u8(key, 0), b = settings_get_u8(key, 1);
    if (a != b) {
        return false;
    }
    *out = a;
    return true;
}

bool sw_saved(uint16_t id, char const *saved_key, uint32_t *out) {
    if (id >= REG_ID_RADIO_WIFI && id <= REG_ID_RADIO_BT) {
        *out = lichen_radio_switch_get((radio_id_t)(id - REG_ID_RADIO_WIFI));
        return true;
    }
    if (id == REG_ID_RADIO_AIRPLANE) {
        *out = lichen_airplane_get();
        return true;
    }
    return saved_key && cfg_stored(saved_key, out);
}

bool sw_sensors_boot_get(void) {
    return sensors_boot_start();
}

void sw_sensors_periods(uint32_t *imu_ms, uint32_t *env_ms) {
    sensors_stats_t s;
    sensors_stats(&s);
    *imu_ms = s.imu_ms;
    *env_ms = s.env_ms;
}

uint8_t sw_r16_get(void) {
    return backlight_r16_saved();
}
