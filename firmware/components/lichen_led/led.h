#pragma once
// The status LED service (#74): blue LED D1 on the M.2 card, P4 GPIO51. The OS owns the pin; apps raise notices
// (lichen_notify.h) and the LED follows: unread = a short pulse every 4 s, an update being received = fast blink.
// Do-not-disturb keeps the notice pulse dark. Not part of the app API.

#include <stdbool.h>
#include "fw_store.h"
#include "led_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

// Board: D1 is driven from GPIO51 through R27. Polarity is not confirmed from the schematic: change this one define
// (1 = high lights the LED) if the bench shows it inverted.
#define LED_GPIO        51
#define LED_ACTIVE_HIGH 1

// Where the setting is kept (the settings service, #235): led opens no NVS. Bound at START_EARLY, before led_start;
// until then the default (on) applies and nothing is saved.
void led_bind_store(fw_get_u8_fn get, fw_set_u8_fn set);
// main.c, once after lichen_notify_start: reads the setting ("status_led", default on), configures the
// pin and starts the 50 ms esp_timer. "fw led".
void led_start(void);
bool led_enabled(void);
void led_set_enabled(bool on);     // Settings › Notifications › Status LED; saved
void led_set_updating(bool on);    // main.c's OTA hooks
void led_set_override(led_override_t o);
led_pattern_t led_current(void);

#ifdef __cplusplus
}
#endif
