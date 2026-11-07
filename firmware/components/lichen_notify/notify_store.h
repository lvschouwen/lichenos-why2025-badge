#pragma once
// Notification settings kept by the settings service (#235): do-not-disturb and the per-source switches. lichen_os binds
// the getter and setter, lichen_notify opens no NVS. Pure, host-tested in host_tests/notify_store_test.c.
#include "fw_store.h"

#define NSTORE_KEY_DND "notify_dnd" // the badge_fw key from before the move; the source keys are nmute_key's "n.<source>"

// No getter (not bound yet) gives the defaults: do-not-disturb off, every source on
bool nstore_dnd_load(fw_get_u8_fn get);
bool nstore_dnd_save(fw_set_u8_fn set, bool on);
// false: the source is off. A source whose key does not fit is always on.
bool nstore_source_load(fw_get_u8_fn get, char const *source);
// false when the source has no key, there is no setter or the write failed
bool nstore_source_save(fw_set_u8_fn set, char const *source, bool on);
