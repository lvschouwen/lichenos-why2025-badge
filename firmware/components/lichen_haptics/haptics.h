#pragma once
// The vibration motor service, OS side (#68). Apps use lichen_haptics.h. Motor M1 is switched by Q4 (BC847, low side)
// from P4 GPIO8 through R49 (not always bridged: on the bench badge no vibration was felt, #68, so it may need a solder bridge): high = motor on.
// The pin is driven low and handed to LEDC PWM (timer 3, channel 7) as early as main.c can; a buzz is one 10 ms
// esp_timer that runs only while it plays (no task, no allocation).

#include "haptics_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAPTICS_GPIO 8

// main.c, right after NVS is up: pin low, settings (NVS badge_fw "haptics" default on, "haptic_str" default high),
// hooks the notification feedback ("vibrate": a toast with feedback buzzes), "fw vib".
void haptics_start(void);
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
