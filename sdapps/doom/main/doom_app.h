#pragma once
// The seam between the app (doom_app.c) and the doomgeneric backend (doomgeneric_lichen.c).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Frees every block the engine still holds (the zone, the screen buffer, the heap it grew): the engine task has ended.
void   doom_free_all(void);
// The bytes the engine holds right now.
size_t doom_held_bytes(void);
// The engine task is about to leave through exit() (the menu's Quit, I_Error on a bad WAD): called by the glue first, so
// stop does not wait for a task flag that exit() would never set.
void   doom_engine_ending(void);
// Runs the engine's exit functions (saves its config, stops its subsystems) once the engine is no longer ticking.
void   doom_engine_shutdown(void);
// A close came while the engine still loads the WAD: its reads then get nothing, so it leaves through I_Error at once.
bool   doom_load_cancelled(void);

// #558 spike (build with DOOM_SPIKE=1 or 2, see CMakeLists.txt; absent otherwise): the engine's own work per frame with
// no picture handed to the screen. DG_SleepMs reports the time it waited so the engine's pacing is not counted as work.
#if DOOM_SPIKE
void doom_spike_sleep_enter(void); // DG_SleepMs around its wait
void doom_spike_sleep_leave(void);
#endif
