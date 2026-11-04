#pragma once

#include <stdbool.h>
#include <stddef.h>

// A grid's display order chosen by the person (#189): home's apps now, MeshCore's tiles later. Pure, no ESP headers;
// host-tested in host_tests/tile_order_test.c.
//
// The order is a display order only: order[slot] is an index into the caller's ids[] (home: the app registry), and
// that index stays the thing's identity everywhere else. Stored as text, "1:settings,about,files": the version, a
// colon, then ids comma-separated. Ids are matched exactly; the format leaves room for a later hidden-tile marker.

#define TILE_ORDER_VERSION '1'

// order gets a permutation of 0..n-1: the ids saved finds first, in its order, then every other index in ascending
// (registration) order. An unknown or duplicate id in saved is skipped; NULL, "", a string of another version or one
// that does not parse gives the plain 0..n-1. True when saved was used (a valid string of this version).
bool tile_order_merge(char const *saved, char const *const *ids, int n, int *order);

// Only the ids of saved that are in ids[], each once, in saved's order: found[0..*count). False for NULL, "", another
// version or a malformed string (*count is then 0).
bool tile_order_parse(char const *saved, char const *const *ids, int n, int *found, int *count);

// The text for order (as tile_order_merge reads it back). Returns the length; 0 when it does not fit in cap (out is
// then "") or an id is empty or holds ',' or ':'.
size_t tile_order_encode(char const *const *ids, int const *order, int n, char *out, size_t cap);

// Moves the entry at slot from to slot to, the ones in between shifting up or down a place (insert and shift).
// False (and nothing changes) when from == to or either is out of range.
bool tile_order_move(int *order, int n, int from, int to);

// order is 0..n-1: nothing to store
bool tile_order_is_default(int const *order, int n);

// slot_of[index] = the slot index is shown in
void tile_order_slots(int const *order, int n, int *slot_of);

// One cell size for both tile grids, home's and MeshCore's (#193): they looked alike but sized their cells apart and
// drifted. Three columns of TILE_GRID_ICON_PX icons; the rows are compact enough for MeshCore's three rows plus its hint
// line. Inside a cell, top to bottom: the 3 px border (always there, only its opacity changes), TILE_GRID_PAD_TOP, the
// icon, TILE_GRID_ICON_GAP, the name (one line), then a bottom pad that holds arrange mode's floating "Moving" (one line
// plus TILE_GRID_MOVING_EDGE), so the icon never moves when it shows.
#define TILE_GRID_ICON_PX     64
#define TILE_GRID_COL_GAP     24
#define TILE_GRID_ROW_GAP     16
#define TILE_GRID_BORDER_W    3
#define TILE_GRID_PAD_TOP     8
#define TILE_GRID_ICON_GAP    12
#define TILE_GRID_MOVING_EDGE 4 // "Moving" off the cell's bottom edge, clear of the focus border

typedef struct {
    int w, h;       // the cell's outer size
    int pad_bottom; // under the name, for "Moving"
} tile_cell_t;

// The cell for a grid of cols columns across avail_w pixels (the screen minus its side pads), names in a font of
// line_h. Sizes from the font, so a taller font never pushes the name out of the cell (#28).
tile_cell_t tile_cell_size(int avail_w, int cols, int line_h);
