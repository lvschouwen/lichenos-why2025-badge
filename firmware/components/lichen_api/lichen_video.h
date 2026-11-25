#pragma once
// Full-screen video for an app (#562, part of #527; CAP_VIDEO): the app renders RGB565 frames into two OS-owned source
// buffers and the OS scales and rotates them onto the panel (the PPA), centred with black bars. The geometry is the OS's
// (a 320 x 200 source fills 720 x 537), LVGL is paused while the app holds the screen, and the status bar keeps being
// drawn. Calls are checked against the app's manifest: an app without VIDEO is refused (false / NULL, counted in
// "fw app caps"). One holder at a time; the OS releases what an unloaded app held. Only the holder may call present,
// back and release, from its open hook or from tasks it started with lichen_task_start (any other task is the OS's
// context and is refused). Use one render task: back/present keep an unlocked frame index. Stop that task before
// release(): release frees the buffers.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Why the OS took the screen back (the values follow the display's, lichen_video_rules.h pins them)
typedef enum {
    LICHEN_VIDEO_RECLAIM_DIALOG = 1, // a critical dialog
    LICHEN_VIDEO_RECLAIM_THERMAL,    // thermal warning
    LICHEN_VIDEO_RECLAIM_CARD,       // the SD card was removed
    LICHEN_VIDEO_RECLAIM_ESCAPE,     // the person held Esc
    LICHEN_VIDEO_RECLAIM_ENGINE,     // the scaler was lost
} lichen_video_reclaim_t;

// Runs on the LVGL task under the UI lock: it must not block or wait on anything the UI could hold (set a flag, notify a
// task). The screen is already the OS's again; present() then returns false until the app calls lichen_video_release().
typedef void (*lichen_video_reclaim_fn)(void *arg, lichen_video_reclaim_t why);

typedef struct {
    uint16_t                src_w, src_h; // 320 x 200 for the DOOM geometry; one that does not fit the panel is refused
    lichen_video_reclaim_fn on_reclaim;   // may be NULL
    void                   *arg;
} lichen_video_cfg_t;

// LVGL context (the app's open hook). Takes the screen: allocates two black src_w x src_h RGB565 buffers in PSRAM (64-byte
// aligned, the OS's, freed by release or unload). False when the app has no VIDEO, cfg is invalid, another app holds the
// screen, the scaler or the PSRAM speed does not allow it, or memory is short: the app then keeps its LVGL path.
bool lichen_video_acquire(lichen_video_cfg_t const *cfg);
// The buffer to render the next frame into (the first is the second source: the first one shows black until the first
// present). NULL when the app does not hold the screen. Stays the same after a dropped present.
uint16_t *lichen_video_back(void);
// Any task, never blocks: shows the frame in lichen_video_back() and flips to the other buffer. False = dropped (the
// previous frame is still pending, or the screen was taken back); render the frame again into the same buffer.
bool lichen_video_present(void);
// Gives the screen back (LVGL context; idempotent). False only when the UI lock was not free in time: call again.
bool lichen_video_release(void);

#ifdef __cplusplus
}
#endif
