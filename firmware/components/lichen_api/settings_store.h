#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lichen's own settings in NVS namespace badge_fw (#66): home_layout (u8), tz (str), setup_done (u8), idle_dim and
// idle_off (u8, tens of seconds, #43), saver and lock (u8 0/1, #75), saver_style (u8, #119),
// home_order (str, #189: tile_order.h's text). Not for apps:
// they have lichen_settings.h (#84). A missing key or
// an unreadable NVS gives the default. Keys are at most 15 characters (NVS limit).
// A factory reset (#66) erases every badge_fw key except the badge's own record listed in lichen_boot/factory_plan.c:
// a new key that is not a person's setting belongs on that list.
uint8_t settings_get_u8(char const *key, uint8_t def);
bool    settings_set_u8(char const *key, uint8_t value);
// Copies the value (or def) into out, always NUL-terminated
void    settings_get_str(char const *key, char *out, size_t cap, char const *def);
bool    settings_set_str(char const *key, char const *value);
// Removes a key; true when it is gone (also when it was never there)
bool    settings_erase(char const *key);
// Applies the saved time zone (default Amsterdam) to the C library: setenv("TZ") and tzset(). Before the clock shows
void    settings_apply_tz(void);
// Saves and applies a POSIX TZ string
bool    settings_set_tz(char const *posix);
