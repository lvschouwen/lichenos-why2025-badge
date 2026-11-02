#pragma once
// Updates over Wi-Fi closing on their own (#44, #110): optional, off by default. With a time set, the update server
// closes that many minutes after boot or after it was switched on, for the rest of this boot; the setting itself stays
// on, so the next boot opens it again. Pure, host-tested in host_tests/ota_autooff_test.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_AUTOOFF_MAX_MIN 240 // the console takes 1..240 minutes; Settings steps through the choices below

// Settings' choices: 0 (never) -> 10 -> 30 -> 60 -> 120 -> 0; anything else steps to the next choice above it
uint8_t ota_autooff_next(uint8_t minutes);
// "Never", "10 min", "2 h"
void    ota_autooff_text(uint8_t minutes, char *out, size_t cap);
// "off" (0) or 1..OTA_AUTOOFF_MAX_MIN minutes; false on anything else
bool    ota_autooff_parse(char const *arg, uint8_t *minutes);
// Time to close: a time is set, it has passed since armed_s (uptime in seconds at boot or at the switch-on), and no
// upload is in progress (an upload in pieces spans connections: closing would drop it half-way)
bool    ota_autooff_due(uint8_t minutes, uint32_t armed_s, uint32_t now_s, bool busy);
// Seconds left until due (0 when due or never)
uint32_t ota_autooff_left(uint8_t minutes, uint32_t armed_s, uint32_t now_s);
