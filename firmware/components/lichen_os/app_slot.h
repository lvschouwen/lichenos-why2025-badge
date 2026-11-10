#pragma once

// A per-app table's slot lookup (#272): lichen_fs keeps one directory per app in a fixed table, and an app asks for
// its directory from its own task, so the lookup and the claim run under the table's mutex. Pure (no ESP-IDF),
// host-tested in host_tests/app_slot_test.c.

#include <stddef.h>

// The index of app's slot in owners[0..n), else of the first free (NULL) one; n when the app has none and the table
// is full. The caller claims a free slot by writing app into it, after whatever makes the slot usable succeeded.
size_t app_slot_pick(void const *const owners[], size_t n, void const *app);
