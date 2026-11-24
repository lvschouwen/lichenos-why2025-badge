#pragma once
// The seam between the app (doom_app.c) and the doomgeneric backend (doomgeneric_lichen.c).
#include <stdbool.h>
#include <stddef.h>

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
