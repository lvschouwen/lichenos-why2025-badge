#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Lichen's own settings in NVS namespace badge_fw (#66): home_layout (u8), tz (str), setup_done (u8), idle_dim and
// idle_off (u8, tens of seconds, #43). Not for apps:
// they have lichen_settings.h (#84). A missing key or
// an unreadable NVS gives the default. Keys are at most 15 characters (NVS limit).
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
