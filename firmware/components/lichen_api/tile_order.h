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
