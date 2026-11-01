#pragma once
// The crash log for apps (#72): the crashes Lichen recorded (crash.c keeps the last CRASHLOG_MAX), newest first, and
// the viewer's "new since last opened" marker. Apps never read /storage/crash or the crash NVS keys themselves.
// Index 0 is the newest entry. Calls read the storage drive (a few KB) and NVS: not from a tile_text every second, cache
// and watch lichen_crashlog_generation() instead. Any task, but one at a time (the Crash log app runs them on LVGL's).

#include "crashlog_fmt.h"
#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Up to max entries into out, newest first; returns how many
size_t lichen_crashlog_list(crashlog_entry_t *out, size_t max);
// One entry; false when there is no such index
bool lichen_crashlog_read(size_t index, crashlog_entry_t *out);
// Deletes entry index if it still is expect (its line): the log is rewritten without it. The app's seen marker moves
// to the new newest entry, so the deletion does not count as news. False when it is gone or the write failed.
bool lichen_crashlog_delete(lichen_app_t const *app, size_t index, crashlog_entry_t const *expect);
// Everything "fw crash clear" clears (the log, the crash counter, the core dump) and the app's seen marker. False when
// a part failed.
bool lichen_crashlog_clear(lichen_app_t const *app);

typedef struct {
    size_t           entries;
    size_t           unseen; // entries new since the app last called lichen_crashlog_mark_seen
    crashlog_entry_t newest; // valid when entries > 0
} lichen_crashlog_status_t;
void lichen_crashlog_status(lichen_app_t const *app, lichen_crashlog_status_t *out);
// The app was opened: what is stored now is no longer new
void lichen_crashlog_mark_seen(lichen_app_t const *app);
// Changes whenever the log is written or cleared (also from the console)
uint32_t lichen_crashlog_generation(void);
// This boot follows a crash
bool lichen_crashlog_after_crash(void);

#ifdef __cplusplus
}
#endif
