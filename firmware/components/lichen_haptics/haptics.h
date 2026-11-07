#pragma once
// The vibration motor service, OS side (#68). Apps use lichen_haptics.h. Motor M1 is switched by Q4 (BC847, low side)
// from P4 GPIO8 through R49 (not always bridged: on the bench badge no vibration was felt, #68, so it may need a solder bridge): high = motor on.
// The pin is driven low and handed to LEDC PWM (timer 3, channel 7) as early as main.c can; a buzz is one 10 ms
// esp_timer that runs only while it plays (no task, no allocation).

#include "fw_store.h"
#include "haptics_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAPTICS_GPIO 8

// Where the settings are kept (the settings service, #235): haptics opens no NVS. Bound at START_EARLY, before
// haptics_start; until then the defaults apply and nothing is saved.
void haptics_bind_store(fw_get_u8_fn get, fw_set_u8_fn set);
// main.c, right after NVS is up: pin low, settings ("haptics" default on, "haptic_str" default high), "fw vib".
void haptics_start(void);
// A notification toast with feedback (#62): lichen_os registers this as lichen_notify's vibrate hook (#245), so it runs
// on the LVGL task. level is lichen_notify.h's ln_level_t (0 info, 1 warning, 2 error; hp_for_notice picks the
// pattern). Nothing before haptics_start worked; at most one buzz per HP_NOTIFY_GAP_MS.
void haptics_notice(uint8_t level);
void haptics_play(hp_pattern_t p);          // honours the setting and updates; replaces a running buzz
void haptics_force(hp_pattern_t p);         // "fw vib test": ignores the rate limit but not the setting
void haptics_stop(void);                    // motor off now (power off / restart)
void haptics_set_updating(bool on);         // main.c's OTA hooks: silent while an update is received
bool haptics_playing(void);
// The person's setting (default on, strength high), saved; lichen_os's lichen_haptics.h calls these (#221)
bool          haptics_enabled(void);
void          haptics_set_enabled(bool on); // off stops a buzz at once
hp_strength_t haptics_strength(void);
void          haptics_set_strength(hp_strength_t s); // out of range: ignored

#ifdef __cplusplus
}
#endif
