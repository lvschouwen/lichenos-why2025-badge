#pragma once
// The crash log for apps (#72): the crashes Lichen recorded (crash.c keeps the last CRASHLOG_MAX), newest first, and
// the viewer's "new since last opened" marker. Apps never read /storage/crash or the crash NVS keys themselves.
// Index 0 is the newest entry. Calls read the storage drive (a few KB) and NVS: not from a tile_text every second, cache
// and watch lichen_crashlog_generation() instead. Any task; delete and clear write flash and NVS (tens to hundreds of ms),
// so not on the LVGL task. Rewrites hold crash.c's log lock, so "fw crash clear" cannot interleave.
//
// The calls that keep a seen marker (delete, clear, status, mark_seen) take the caller's own ctx (#230 E1c): the marker
// is that app's setting. A stale or made-up ctx is refused (logged at most once per 10 s, counted in "fw app ctx",
// never an abort): nothing is read, deleted or cleared and the call returns false. So is the OS's own ctx, which an
// app can reach through lichen_ctx_self() outside its hooks: the OS clears the log from the console ("fw crash clear").

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
// Deletes entry index if it still is expect (NULL: nothing is deleted) (its line): the log is rewritten without it. The app's seen marker moves
// to the new newest entry, so the deletion does not count as news. False when it is gone or the write failed.
bool lichen_crashlog_delete(lichen_ctx_t ctx, size_t index, crashlog_entry_t const *expect);
// Everything "fw crash clear" clears (the log, the crash counter, the core dump) and the app's seen marker. False when
// a part failed.
bool lichen_crashlog_clear(lichen_ctx_t ctx);

typedef struct {
    bool             ok; // false: the log could not be read (no memory); the rest is zero, not "no crashes"
    size_t           entries;
    size_t           unseen; // entries new since the app last called lichen_crashlog_mark_seen
    crashlog_entry_t newest; // valid when entries > 0
} lichen_crashlog_status_t;
bool lichen_crashlog_status(lichen_ctx_t ctx, lichen_crashlog_status_t *out); // out->ok
// The app was opened: what is stored now is no longer new. False (logged once) when the log could not be read.
bool lichen_crashlog_mark_seen(lichen_ctx_t ctx);
// The log as the text export of #170 (crashlog_export): GET /crashes and the SD copy. Any task; returns the length, 0 when
// the log could not be read or cap is too small.
size_t lichen_crashlog_export(char *out, size_t cap);
// Writes that text to the SD card as /sd/lichen/crashes-<date or count>.txt (name copied to path). Not on the LVGL task.
// false: no card, not enough memory or the write failed (reason in the log).
bool lichen_crashlog_export_sd(char *path, size_t cap);
// Changes whenever the log is written or cleared (also from the console)
uint32_t lichen_crashlog_generation(void);
// This boot follows a crash
bool lichen_crashlog_after_crash(void);

#ifdef __cplusplus
}
#endif
