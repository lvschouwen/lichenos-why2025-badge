#pragma once
// Notifications (#62): any app, and Lichen itself, raises a notice; the OS keeps a history of 48 (kept across
// reboots in /storage/lichen/notify.bin), shows a toast over the current screen that never takes the focus, puts the
// unread count on the source app's home tile (lichen_badge.h) and an unread dot in the status bar, and lists them all
// in the notification centre (the Notifications tile, the cloud key). Part of the app API: lichen_api REQUIRES it.
//
// Every call here is safe from any task (never from an ISR); the strings are copied. A source is the app's id
// ("meshcore") or "lichen" for the OS; target is the app's own place key ("ch:badgetest", "dm:<pubkey prefix>"), which
// comes back to the app's lichen_app_t.open_notification when the user opens the notice.
//
// Rate limit: 5 notices in a row per source, then one per 10 s. A notice over the limit still lands in the history
// (merged into an unread one of the same target when it can) but gets no toast. Do-not-disturb (Settings ›
// Notifications) hides toasts and feedback; errors still toast, silently. Errors stay until read: the history drops
// read entries first, then the oldest info, then warnings.
//
// The tile badge (lichen_badge.h) of an app that raises notices belongs to this service: it sets it to the app's
// unread count, so that app must not also call lichen_badge_set for itself (MeshCore moves over in #50).
//
// Follow-up: source is taken on trust; binding it to the calling app (so one app can't post as another) needs the app
// identity the OS does not track per task yet.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LN_INFO, LN_WARN, LN_ERROR } ln_level_t;

#define LN_SILENT    0x01 // history and tile badge only: no toast, no sound/vibration/LED
#define LN_COALESCE  0x02 // merge into an unread entry with the same (source, target) of the last 10 min (count goes up)
#define LN_ONCE_BOOT 0x04 // at most once per boot per (source, target); a repeat returns 0 and is not counted dropped

// title: up to 39 bytes, body up to 119 (cut on a UTF-8 boundary is the caller's care: keep them short). Returns the
// entry's id, 0 when it was dropped (no room, LN_ONCE_BOOT repeat, the source switched off in Settings, or the
// service is not started yet).
uint32_t lichen_notify(char const *source, ln_level_t level, char const *title, char const *body, uint32_t flags);
uint32_t lichen_notify_to(char const *source, ln_level_t level, char const *target, char const *title,
                          char const *body, uint32_t flags);

// Unread entries of source (NULL: all sources)
size_t     lichen_notify_unread(char const *source);
// The highest unread level, for the status bar's colour; LN_INFO when nothing is unread (check lichen_notify_unread)
ln_level_t lichen_notify_unread_top(void);
// The app's own "seen" (MeshCore opened the chat): marks the unread entries of (source, target) read; target NULL =
// all of the source's
void       lichen_notify_mark_read(char const *source, char const *target);
// The user lets this source's notices through (Settings › Notifications, default on). When off, lichen_notify()
// drops them and returns 0; an app need not check first.
bool       lichen_notify_source_enabled(char const *source);
// Do-not-disturb is on (Settings › Notifications)
bool       lichen_notify_dnd(void);

#ifdef __cplusplus
}
#endif
