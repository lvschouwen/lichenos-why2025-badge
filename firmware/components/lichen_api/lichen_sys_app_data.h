#pragma once

// What Settings > Apps (#233) asks the OS about one app and may do to it: its data, its background switch and its crash
// count. Everything is for the app at an index of the OS's table (lichen_sys_app.h's lichen_app_at), never a path or an
// id from outside, so Clear data can only reach the selected app's own data: its settings namespace "a.<id>", its
// directory on the storage drive and its directory on the SD card (lichen_sys_data_fs.h builds both from the id by fs_path.h's
// rules). System surface: tools/layers.conf forbids lichen_sys_* in L6, an app never clears another's data or its own
// switch.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_data_types.h"
#include "lichen_app.h"

#ifdef __cplusplus
extern "C" {
#endif

// lichen_settings.c: how many NVS entries ctx's app namespace holds, none for a namespace never written (true); false
// for a refused or OS ctx or an NVS that cannot be read. The measure's input; reads no value.
bool lichen_sys_app_settings_entries(lichen_ctx_t ctx, size_t *entries);

// The size of the app's data. File and NVS work: never on the LVGL task (the shell runs it on a job task). The SD card
// is read under its batch lock, waiting at most 3 s for it. False for an index past the table.
bool lichen_sys_app_data_measure(size_t index, lichen_app_data_t *out);

// Clear data: the app's settings (its secrets too) and the contents of its two directories. Never an app whose
// descriptor says storage_secret (MeshCore's identity and keys: only a factory reset erases those, false and nothing
// touched), never the OS's own context. A running app keeps what it holds in RAM and may write settings back: the page
// says so. Not on the LVGL task (file and settings writes). Only the shell calls it (tools/layers.conf).
bool lichen_sys_app_data_clear(size_t index, lichen_app_clear_t *out);

// The background switch (#233 on top of #232's list). _allowed: whether the app's background part may start at the next
// boot (it is not on the list of switched-off apps). Any task.
bool lichen_sys_app_background_allowed(size_t index);
// The change: saved, and applied from the next boot; an app is never stopped by it (the lifecycle has no stop for it).
// Turning it on forgets the app's crashes, as "fw app background <id> on" does. False (nothing changes, the list in RAM
// included, so the row shows what is saved) for an index past the table, an app without a background part, one the OS
// keeps on (lichen_app_t.safe_mode_exempt) or a list that could not be saved. Not on the LVGL task (a settings write).
// Only the shell calls it (tools/layers.conf).
bool lichen_sys_app_background_set(size_t index, bool allowed);

// How many of the last *window boots ended in a crash named after the app (either kind, #232/#427); 0 and *window 0 for
// an index past the table. Any task.
unsigned lichen_sys_app_crashes(size_t index, unsigned *window);

#ifdef __cplusplus
}
#endif
