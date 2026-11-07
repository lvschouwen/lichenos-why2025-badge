#pragma once
// The vibration setting (#68), kept by the settings service (#235): lichen_os binds the getter and setter, haptics.c
// opens no NVS. Pure, host-tested in host_tests/haptics_setting_test.c.
#include "fw_store.h"
#include "haptics_pattern.h"

#define HAPTICS_KEY_ENABLED  "haptics"    // the badge_fw keys from before the move
#define HAPTICS_KEY_STRENGTH "haptic_str"

// Enabled (default on) and strength (default high; an out-of-range saved value is high). No getter gives the defaults.
void haptics_setting_load(fw_get_u8_fn get, bool *enabled, hp_strength_t *strength);
bool haptics_setting_save_enabled(fw_set_u8_fn set, bool on);
bool haptics_setting_save_strength(fw_set_u8_fn set, hp_strength_t s);
