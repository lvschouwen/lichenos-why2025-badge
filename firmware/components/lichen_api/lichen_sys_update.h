#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Wi-Fi update server's system surface (#227, #216 decision 1): Settings > Updates over Wi-Fi goes through here,
// never to the ota component. System surface: tools/layers.conf forbids lichen_sys_* in L6; the server itself is
// started and hooked by main.

// Off closes the update port (and with it the Wi-Fi console); saved in NVS. False while refused, whatever the setting
// says. Any task.
bool lichen_sys_update_enabled(void);
// No token read or saved anywhere this boot: the server does not listen this boot. Any task.
bool lichen_sys_update_refused(void);
// Switches it on or off; on also starts the auto-off time again. Saved to NVS. Any task but not an interrupt.
void lichen_sys_update_set_enabled(bool on);
// Optional auto-off: minutes after boot or switch-on, 0 = never. Any task.
uint8_t lichen_sys_update_auto_off(void);
// Saved in NVS; a new value counts from now. Any task but not an interrupt.
void lichen_sys_update_set_auto_off(uint8_t minutes);
// The auto-off closed it this boot (lichen_sys_update_enabled is false then). Any task.
bool lichen_sys_update_auto_closed(void);
// Settings' choices: 0 (never) -> 10 -> 30 -> 60 -> 120 -> 0; anything else steps to the next choice above it. Any task.
uint8_t lichen_sys_update_auto_off_next(uint8_t minutes);
// "Never", "10 min", "2 h". Any task.
void lichen_sys_update_auto_off_text(uint8_t minutes, char *out, size_t cap);
