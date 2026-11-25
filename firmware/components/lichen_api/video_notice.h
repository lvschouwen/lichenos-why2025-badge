#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a notice does while a video owner holds the screen (#563, part of #527; owner decision 2026-11-25): ordinary toasts
// are held (the status-bar unread dot still counts them, they show on exit); a dialog, the thermal warning, the card's
// removal and the hold-Esc take the screen back at once. Pure policy plus two pieces of state the shell (L5) and
// lichen_os (L3) share without including each other. Host-tested in host_tests/video_notice_test.c.

typedef enum { VN_TOAST, VN_DIALOG, VN_THERMAL, VN_CARD, VN_ESCAPE } vn_kind_t;
typedef enum { VN_SHOW, VN_HOLD, VN_RECLAIM } vn_action_t;

// What to do with a notice of this kind. Pure; no owner means every kind just shows. Any task.
vn_action_t video_notice_policy(vn_kind_t kind, bool owner_active);

// lichen_os keeps these two: the owner is ACTIVE (set under the UI lock on acquire, cleared on reclaim and release), and
// how to take the screen back (video_owner_reclaim). Any task.
void video_notice_set_active(bool active);
// Any task.
void video_notice_set_reclaim(bool (*reclaim)(vn_kind_t kind));
// Whether a video owner is active now. Any task.
bool video_notice_active(void);
// A notice of this kind is raised: reclaims when the policy says so. True when the screen was taken back. Any task; the
// reclaim waits for the UI lock (bounded), so not from the keyboard task.
bool video_notice_raise(vn_kind_t kind);

// The hold-Esc escape (2 s, enforced by the OS so a hung engine cannot trap the user). The keyboard task counts presses
// and says whether Esc is down; the LVGL task samples them (timer, independent of what any app does with its keys).
// A short Esc never reaches this: it is just a key. Plain data, touched by the sampler only.
#define VIDEO_ESCAPE_MS 2000
typedef struct {
    bool     was_active;
    bool     armed;
    bool     fired;
    uint32_t seen; // presses counted when the hold started (or when no owner was active)
    uint32_t since;
} video_escape_t;
// True exactly once per hold: Esc pressed after the owner became active and kept down VIDEO_ESCAPE_MS. An Esc already
// down when the owner starts is no escape. LVGL context (one sampler).
bool video_escape_sample(video_escape_t *e, bool active, bool down, uint32_t presses, uint32_t now);

#ifdef __cplusplus
}
#endif
