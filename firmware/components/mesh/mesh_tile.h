#pragma once

#include "mesh_store.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MeshCore's home tile (#67). Pure: the mesh task keeps a mesh_tile_t up to date, home asks mesh_tile_text() for the two
// lines. Unread = the sum over all chats (#48), cleared per chat while that chat is open.

typedef struct {
    uint32_t unread_total;
    size_t   contacts;
    uint32_t last_heard_age_s;
    char     latest[MESH_TEXT_MAX + 1]; // "[channel] text" of the newest message, "" when none
    bool     advert_sent;  // an advert of ours went out since boot (#88: the grid's Advertise badge)
    uint32_t advert_age_s; // since then
} mesh_tile_t;

#define MESH_TILE_UNREAD_CAP 99 // shown as "99+" above this

// line1 "<n> unread · <contacts> nodes" ("<contacts> nodes" with nothing unread); line2 the latest message cut to fit on
// a UTF-8 boundary, or "Listening on Public" when there is none. The dot is U+2022: the badge font has no U+00B7.
void mesh_tile_text(mesh_tile_t const *t, char *line1, size_t c1, char *line2, size_t c2);
// A message arrived: the newest line (the unread total is set by the caller from the store)
void mesh_tile_on_message(mesh_tile_t *t, char const *channel, char const *text);

#ifdef __cplusplus
}
#endif
