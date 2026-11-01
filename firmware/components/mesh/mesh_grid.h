#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The MeshCore app's own home grid (#88): which tiles, in which order, what their badges say and how the arrow keys
// move the selection. Pure logic, no ESP headers (host_tests/mesh_grid_test.c); meshcore_grid.c draws it.

typedef enum {
    MESH_GRID_CHAT,      // the conversation list (#48)
    MESH_GRID_CONTACTS,  // every contact, details on Enter (#49)
    MESH_GRID_REPEATERS, // contacts that are repeaters or rooms (#51)
    MESH_GRID_HEARD,     // contacts by when they were last heard, newest first
    MESH_GRID_ADVERTISE, // a zero-hop advert, confirmed first (#52)
    MESH_GRID_SETTINGS,  // the app's settings page (#17)
    MESH_GRID_TILES
} mesh_grid_tile_t;

// Three columns, as Lichen's home grid (home_layout.h HOME_GRID_COLS)
#define MESH_GRID_COLS 3
// The key values LVGL's keypad sends (LV_KEY_*); meshcore_grid.c static_asserts them against lvgl.h
#define MESH_GRID_KEY_UP    17
#define MESH_GRID_KEY_DOWN  18
#define MESH_GRID_KEY_RIGHT 19
#define MESH_GRID_KEY_LEFT  20

// The tile's name ("Chat", ...); "" for a value outside the enum
char const *mesh_grid_name(mesh_grid_tile_t tile);

// What the badges are made from: copies the grid takes once a second
typedef struct {
    uint32_t unread;      // over all chats (the home tile's number)
    size_t   contacts;    // contacts that are not repeaters or rooms (the Contacts list, #101)
    size_t   repeaters;   // contacts that are repeaters or rooms
    bool     heard_any;   // a contact has been heard at all
    uint32_t heard_age_s; // since the most recently heard contact
    bool     advert_sent; // an advert went out since boot
    uint32_t advert_age_s;
} mesh_grid_counts_t;

#define MESH_GRID_BADGE_CAP 99 // a count above this reads "99+"
#define MESH_GRID_BADGE_MAX 8  // bytes for a badge with its terminator

// A count as a badge: "" for 0, "7", "99+" above MESH_GRID_BADGE_CAP
void mesh_grid_count_text(size_t n, char *out, size_t cap);
// An age as a badge: "now" under a minute, "5m", "3h", "2d", "99d+" from 100 days
void mesh_grid_age_text(uint32_t age_s, char *out, size_t cap);
// The badge of one tile ("" for none): Chat the unread total, Contacts and Repeaters their counts, Heard the age of
// the newest hearing, Advertise the age of the last advert sent (none before the first), Settings none
void mesh_grid_badge(mesh_grid_tile_t tile, mesh_grid_counts_t const *c, char *out, size_t cap);

// The selection after `key` in a grid of `count` tiles, MESH_GRID_COLS wide: the arrows move in 2-D and stop at the
// edges and where there is no tile, exactly as home_move(HOME_GRID, ...) does (no wrap). Other keys keep `index`; an
// index out of range gives 0.
int mesh_grid_move(int count, int index, uint32_t key);

#ifdef __cplusplus
}
#endif
