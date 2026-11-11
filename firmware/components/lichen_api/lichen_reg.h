#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The settings registry for apps (#313; the registry is #307/#310): one key's last known applied value, read from the
// OS's cache. Read-only on purpose: apps never set a key (the console's `fw set` and the OS's own screens do), and a
// read is not attributed to an app. Safe from any task; it never waits for the C6 (a C6 key shows what the OS last
// heard, with its age).
// buf gets "<value> (<age> ago)", "<value> (stale, <age> ago)" when the C6 stopped answering, "<value>" for a key
// without an age, or "? (<why>)" when there is no value ("? (not read yet)"); cut to fit cap, always NUL-terminated
// when cap > 0. Returns the length written, or 0 for a name the registry does not have (buf is then untouched).
// Names are the `fw get` names ("bl.display", "lora.freq", "wifi.rssi"; shared/reg/reg_ids.h lists them).
size_t lichen_reg_get(char const *name, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
