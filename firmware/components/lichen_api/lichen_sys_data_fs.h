#pragma once
// An app's data on the drives (#233, Settings > Apps): where it lives, how big it is and how it is cleared. Pure POSIX
// (no ESP-IDF, no LVGL), host-tested on a temp directory in host_tests/app_data_fs_test.c; lichen_sys_app_data.h's
// implementation in lichen_os runs it on the storage drive and the SD card. File work: never on the LVGL task.
//
// Clear data may only ever touch the selected app's own data, so the directories are built here from the app's id by
// fs_path.h's rules (one plain component below the base: no "..", no "/", nothing the OS keeps) and the walk never
// follows a link or leaves the directory it was given.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_DATA_PATH_MAX  96 // an app's directory path with its NUL: "/sd/lichen/apps/" + a 13 character id is 30
#define APP_DATA_DEPTH_MAX 6  // directories below the app's own; deeper ones are left alone and counted as skipped

typedef struct {
    uint64_t bytes;   // sum of the plain files' sizes
    uint32_t files;   // plain files seen (measure) or removed (clear)
    uint32_t dirs;    // directories below the app's own seen or removed
    uint32_t skipped; // entries not touched: too deep, a name too long, a link, or a call that failed
} app_data_fs_t;

// The app's directory below base: "<base>/<id>" by fs_path.h's rules (an id that is not an app id, one the OS keeps
// ("c6", "lichen"), a relative base or a path of APP_DATA_PATH_MAX or more gives false and an empty out)
bool lichen_sys_data_dir_path(char const *base, char const *id, char *out, size_t cap);

// The size of everything below dir. A directory that does not exist is no data (true, all zero); false (out as far as
// it got) when dir cannot be read.
bool lichen_sys_data_dir_measure(char const *dir, app_data_fs_t *out);

// Removes everything below dir (files, then the emptied directories) and keeps dir itself. A missing dir is already
// clear (true). False when anything was left (out->skipped says how many entries), so the caller does not claim a clear
// that did not happen. Never follows a link, never goes above dir. Only lichen_os calls it (tools/layers.conf).
bool lichen_sys_data_dir_clear(char const *dir, app_data_fs_t *out);

#ifdef __cplusplus
}
#endif
