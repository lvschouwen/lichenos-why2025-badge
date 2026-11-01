#pragma once
// The notification service's OS side (#62): the shell's toast, centre and status bar, main.c and the console. Not for
// apps (they use lichen_notify.h). Everything is safe from any task unless it says otherwise.

#include "lichen_notify.h"
#include "notify_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

// main.c, once, right after storage_mount (whatever it returned): the mutex, the PSRAM history, the saved file (when
// the drive is up), the do-not-disturb setting, "fw notify". lichen_notify() returns 0 before this.
void lichen_notify_start(bool storage_ok);

// Restart / power off (#65): runs a waiting save at once and waits up to wait_ms for it; true when nothing is left
// to write. Blocks: never on the LVGL task.
bool lichen_notify_save_now(uint32_t wait_ms);

// The clock can be believed (lichen_clock's clock_is_set): entries get a wall-clock time only then. Set once by the shell.
void lichen_notify_set_clock(bool (*is_set)(void));

// Moves on every change of the history: a view reads it again then
uint32_t lichen_notify_generation(void);
// Ids newest first (at most max) and a copy of one entry (false when it is gone)
size_t lichen_notify_ids(uint32_t *ids, size_t max);
bool   lichen_notify_get(uint32_t id, nr_entry_t *out);
void   lichen_notify_mark_read_id(uint32_t id);
void   lichen_notify_mark_read_upto(ln_level_t level); // opening the centre marks info read (Lucas, #62 Q3)
void   lichen_notify_mark_all_read(void);
bool   lichen_notify_remove(uint32_t id);
void   lichen_notify_clear_all(void);
void   lichen_notify_set_dnd(bool on); // NVS badge_fw "notify_dnd"
// Settings › Notifications: a source switched off has its notices dropped (no history, no toast, no badge; counted as
// muted). Kept in NVS badge_fw "n.<source>" (only while off); default on. Entries it already has stay.
void   lichen_notify_set_source_enabled(char const *source, bool on);

// A notice to toast, queued by lichen_notify() for the shell's LVGL timer (the any-task → LVGL hand-off)
typedef struct {
    uint32_t id;
    uint8_t  level;
    bool     feedback; // also sound/vibration/LED (not under do-not-disturb)
} ln_toast_t;
bool lichen_notify_next_toast(ln_toast_t *out); // false when none is waiting

// Sound (#67), vibration (#68), status LED (#74): none yet, every member may be NULL. The shell fires them when it
// shows a toast that has feedback (led on), and when that toast goes (led off).
typedef struct {
    void (*sound)(ln_level_t level);
    void (*vibrate)(ln_level_t level);
    void (*led)(ln_level_t level, bool on);
} ln_feedback_t;
void lichen_notify_set_feedback(ln_feedback_t const *fb);
void lichen_notify_feedback(ln_level_t level, bool on);

typedef struct {
    size_t   entries;
    size_t   unread;
    uint32_t dropped;
    uint32_t muted; // dropped because their source is switched off
    bool     dnd;
    bool     started;
    bool     storage; // the file is used (the drive was up at start)
    uint16_t boot;
    uint32_t saves;
    uint32_t save_errors;
    int64_t  last_save_ms; // uptime, 0 = never this boot
} ln_stats_t;
void lichen_notify_stats(ln_stats_t *out);

#ifdef __cplusplus
}
#endif
