#pragma once
// The settings service's write failures (#320): one saturating counter and the last failure (key, esp_err_t, uptime),
// served as the RO registry keys settings.write_fail and settings.last_err. ESP-free, host-tested in settings_fail_test.c.
// Safe from any task: one spinlock (portMUX) over the ~28-byte state, held for the copy only. Count and text move
// together under it, so the latest failure always wins and a count above 0 never pairs with an empty text.
#include <stdint.h>
#include <stddef.h>

#define SETTINGS_FAIL_KEY_MAX 15 // an NVS key is at most 15 characters

// Count one failed write and remember it as the latest.
void settings_fail_record(char const *key, int err, uint32_t uptime_s);
// settings_fail_record with the uptime read from the clock (0 on the host)
void settings_fail_note(char const *key, int err);

uint32_t settings_fail_count(void);
// "<key> 0x<err>" (the key has no namespace: the store, an app's settings and the backup all report here), at most REG_STR_MAX (15) characters: the error code is whole, the key is cut to fit; "" if none yet
void settings_fail_last_text(char *out, size_t cap);
// The last failure's uptime in seconds and error code (0 and 0 if none yet)
uint32_t settings_fail_last_uptime_s(void);
int      settings_fail_last_err(void);
// Test hooks: forget everything; start the counter at n (to reach the top without 4 billion calls)
void settings_fail_reset(void);
void settings_fail_preset_count(uint32_t n);
