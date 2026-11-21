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
#include "safe_mode.h"
#include "sensors.h"
#include "lora_os.h"
#include "wifi_service.h"

_Static_assert(RADIO_WIFI == 0 && RADIO_LORA == 1 && RADIO_BT == 2, "the rows' radio indexes follow radio_id_t");

_Static_assert(OTA_AUTOOFF_MAX_MIN == 240, "ota.autooff row range (reg_p4_switch.c)");

// The requested state of each driver after the holds, not link state and not the person's switch (#501): Wi-Fi the
// service's enabled flag, LoRa the driver's enabled switch (the radio listener sets both synchronously, a thermal hold
// included). Whether Wi-Fi is joined or LoRa is receiving is wifi.* / lora.state. Bluetooth has no driver yet (#503), so
// it is the OS switch.
bool sw_radio_get(unsigned radio) {
    switch (radio) {
        case RADIO_WIFI: return wifi_enabled();
        case RADIO_LORA: return lora_enabled();
        case RADIO_BT: return lichen_radio_get(RADIO_BT);
        default: return false;
    }
}

bool sw_radio_saved(unsigned radio) {
    return radio < RADIO_COUNT && lichen_radio_switch_get((radio_id_t)radio);
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
    return haptics_set_enabled(on); // false: not saved
}

uint8_t sw_haptics_strength_get(void) {
    return (uint8_t)haptics_strength();
}

bool sw_haptics_strength_put(uint8_t strength) {
    return haptics_set_strength((hp_strength_t)strength);
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

bool sw_ota_put(bool on) {
    return ota_server_set_enabled(on);
}

uint8_t sw_ota_autooff_get(void) {
    return ota_server_auto_off();
}

bool sw_ota_autooff_put(uint8_t minutes) {
    return ota_server_set_auto_off(minutes);
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

uint8_t sw_bg_off_count(void) {
    return (uint8_t)safe_mode_background_off_count();
}

bool sw_led_get(void) {
    return led_enabled();
}

bool sw_led_put(bool on) {
    return led_set_enabled(on);
}
