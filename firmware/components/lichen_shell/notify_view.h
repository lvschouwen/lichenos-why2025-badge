#pragma once
// What the shell shows of notifications (#62): toast timing and its pending queue, the row texts of the centre, the
// status bar's count. Pure, host-tested in host_tests/notify_view_test.c; notify_ui.c and notify_centre.c draw it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// How long a toast stays: info 3 s, warning 6 s, error 10 s
uint32_t nview_toast_ms(uint8_t level);

// Toasts waiting while one is showing: a newer one for the same entry (a coalesced notice keeps its id) replaces the
// waiting one; a full queue drops its oldest of the lowest level, or the newcomer when that is lower still
#define NVIEW_QUEUE 4
typedef struct {
    uint32_t id;
    uint8_t  level;
    bool     feedback;
} nview_toast_t;
typedef struct {
    nview_toast_t q[NVIEW_QUEUE];
    size_t        n;
} nview_queue_t;
// False when t was not queued (it was dropped)
bool nview_push(nview_queue_t *q, nview_toast_t t);
bool nview_pop(nview_queue_t *q, nview_toast_t *out);
// The entry was read or removed: its waiting toast goes
void nview_forget(nview_queue_t *q, uint32_t id);

// A row's title: "Clock answer ignored", or "#badgetest (×5)" for an entry that merged 5 notices
void nview_title(char *out, size_t cap, char const *title, unsigned count);
// How old an entry is: "now", "4 min", "2 h", "3 d" within this boot (uptime) or across boots when both wall clocks
// are known (epoch 0 = unknown), else "earlier boot"
void nview_age(char *out, size_t cap, uint16_t boot, uint32_t mono_s, int64_t epoch, uint16_t now_boot,
               uint32_t now_mono_s, int64_t now_epoch);
// The status bar's count: "" at 0 (hidden), "7", "99+"
void nview_count(char *out, size_t cap, size_t unread);

#ifdef __cplusplus
}
#endif
