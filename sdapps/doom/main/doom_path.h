#pragma once
// Which way the DOOM picture goes (#564): the OS video surface (lichen_video.h) or, when the OS refuses it, the #526 LVGL
// canvas. The decisions of that choice, without the ESP, so host_tests/doom_path_test.c runs them. The LVGL task writes the
// state (open, reclaim, resume); the engine task only reads it. GPL-3.0-or-later, like the app as a whole.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DOOM_PATH_PENDING = 0, // loading: the first frame has not come, nothing is chosen
    DOOM_PATH_VIDEO,       // the OS surface is held, the engine renders into it
    DOOM_PATH_CANVAS,      // the OS refused the surface: the LVGL canvas of #526
    DOOM_PATH_PAUSED,      // the OS took the screen back: the engine waits, the screen offers to resume
} doom_path_state_t;

// lichen_video_reclaim_t's ESCAPE value (lichen_video.h; doom_ui.c asserts it): the OS's own hold-Esc is leaving the app
#define DOOM_RECLAIM_ESCAPE 4

typedef struct {
    atomic_int state; // doom_path_state_t
    atomic_int why;   // the reclaim reason while paused
} doom_path_t;

void              doom_path_init(doom_path_t *p);
doom_path_state_t doom_path_get(doom_path_t *p);
// The first frame came and the screen tried the OS surface. PENDING (or PAUSED, a resume) becomes VIDEO or CANVAS.
void              doom_path_chosen(doom_path_t *p, bool video_acquired);
// on_reclaim: VIDEO becomes PAUSED. False in any other state (a stale call changes nothing).
bool              doom_path_reclaimed(doom_path_t *p, int why);
int               doom_path_why(doom_path_t *p); // the reclaim reason while paused
// Whether the app's own hold-Esc may call shell_back. While the OS surface is held, or was just reclaimed for the OS's own
// escape, the OS pops the app itself (#563): a second back would leave the screen under it too. The canvas path and a pause
// for another reason (a dialog, the thermal warning) have no OS escape: the app's hold is the way out.
bool              doom_path_own_back_ok(doom_path_t *p);
// The engine renders a frame in this state
bool              doom_path_engine_draws(doom_path_t *p);

// The game's clock without the paused time (engine task only, no atomics): the engine's tic loop would otherwise run the
// whole pause in one burst at the resume. Times are wrapping ms ticks.
typedef struct {
    uint32_t begin; // tick the pause began
    uint32_t total; // ms paused so far
    bool     held;
} doom_clock_t;

// Every engine loop: is the game held now
void     doom_clock_held(doom_clock_t *c, uint32_t now, bool held);
// The game's ms at now (frozen across a pause)
uint32_t doom_clock_ms(doom_clock_t const *c, uint32_t now);
