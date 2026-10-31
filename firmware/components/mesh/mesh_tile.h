#pragma once

#include "mesh_store.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MeshCore's home tile (#67). Pure: the mesh task keeps a mesh_tile_t up to date, home asks mesh_tile_text() for the two
// lines. Unread = generation - seen_generation (mesh_store generations: one per message).

typedef struct {
    uint32_t generation, seen_generation;
    size_t   contacts;
    uint32_t last_heard_age_s;
    char     latest[MESH_TEXT_MAX + 1]; // "[channel] text" of the newest message, "" when none
} mesh_tile_t;

#define MESH_TILE_UNREAD_CAP 99 // shown as "99+" above this

// line1 "<n> unread · <contacts> nodes" ("<contacts> nodes" with nothing unread); line2 the latest message cut to fit on
// a UTF-8 boundary, or "Listening on Public" when there is none. The dot is U+2022: the badge font has no U+00B7.
void mesh_tile_text(mesh_tile_t const *t, char *line1, size_t c1, char *line2, size_t c2);
// Messages has been seen: nothing unread any more
void mesh_tile_mark_seen(mesh_tile_t *t);
// A message arrived: the newest line and the generation it brought
void mesh_tile_on_message(mesh_tile_t *t, char const *channel, char const *text, uint32_t generation);

#ifdef __cplusplus
}
#endif
