#pragma once

#include <stdbool.h>
#include <stddef.h>

// The secrets key class of Lichen's settings (#235): values that must never leave the badge in readable form. Today the
// Wi-Fi passwords (badge_fw wifi_pass, the pair in use, and wifi_p0..wifi_p4, the saved networks of #160); the SSIDs
// are names the screens show and stay plain settings. A secret lives in badge_fw like any setting (so #116's NVS
// encryption covers it and a factory reset erases it), but:
//  - only settings_secret_get/set read and write it: settings_get_str/set_str refuse a secret key;
//  - it is never in the backup file's key table (settings_backup.c) nor on the factory reset's keep list
//    (factory_plan.c), so it is never exported and never carried through RAM by a reset (settings_secret_test.c);
//  - no console command prints a value; callers never log one.
// Not for apps: lichen_os's own header, bound into lichen_net by settings_bind_net() (lichen_os.h).

// True for a key of the secrets class in namespace ns. Pure, ESP-free (settings_secret_keys.c).
bool        settings_key_secret(char const *ns, char const *key);
// The class itself, for the tests that pin it out of the backup and the reset's keep list
size_t      settings_secret_count(void);
char const *settings_secret_at(size_t index);

// Reads a secret into out (always terminated, emptied on failure); false when the key is not a secret, missing, too
// long for cap or NVS cannot be read. An empty value (an open network's password) reads back as true.
bool settings_secret_get(char const *key, char *out, size_t cap);
// Saves a secret and commits; false when the key is not a secret or the write fails. settings_erase() removes one.
bool settings_secret_set(char const *key, char const *value);
