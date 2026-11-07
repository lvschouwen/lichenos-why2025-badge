#pragma once
// The status LED's saved switch (#74), kept by the settings service (#235): lichen_os binds the getter and setter, led.c
// opens no NVS. Pure, host-tested in host_tests/led_setting_test.c.
#include "fw_store.h"

#define LED_KEY_ENABLED "status_led" // the badge_fw key from before the move

// On unless a saved 0; a missing key or no getter (not bound yet) is on
bool led_setting_load(fw_get_u8_fn get);
// false when there is no setter or the write failed
bool led_setting_save(fw_set_u8_fn set, bool on);
