#pragma once
// The seam between the app (doom_app.c) and the doomgeneric backend (doomgeneric_lichen.c).
#include <stddef.h>

// Frees every block the engine still holds (the zone, the screen buffer, the heap it grew): the engine task has ended.
void   doom_free_all(void);
// The bytes the engine holds right now.
size_t doom_held_bytes(void);
