#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Keeps a rebuilt list where the reader was (#67), pure (host_tests/scroll_anchor_test.c). Before the rebuild the
// first visible items are noted by identity with their distance from the view's top; afterwards the first of them
// still in the list sets the new scroll position. A pixel offset alone jumps when an item above is dropped (a full
// message window) or the order changes (contacts).

#define SCROLL_ANCHOR_KEYS 4

// An item's place in the list's content (any origin that does not move with scrolling) and its identity
typedef struct {
    int32_t  top, bottom;
    uint32_t key;
} scroll_item_t;

typedef struct {
    size_t   n;
    uint32_t key[SCROLL_ANCHOR_KEYS];
    int32_t  offset[SCROLL_ANCHOR_KEYS]; // item top minus scroll_y
} scroll_anchor_t;

// Items in list order; the first visible one is the first whose bottom lies below scroll_y
scroll_anchor_t scroll_anchor_take(scroll_item_t const *items, size_t n, int32_t scroll_y);
// true and *scroll_y set when an anchored item is still in the list; false (and *scroll_y untouched) otherwise
bool scroll_anchor_find(scroll_anchor_t const *a, scroll_item_t const *items, size_t n, int32_t *scroll_y);

// FNV-1a over len bytes, continuing from seed (0 to start): an identity for items that have no id of their own
uint32_t scroll_key_hash(void const *data, size_t len, uint32_t seed);

// Where a list scrolls to when a key asks to move it by dy (positive = content moves down, as lv_obj_scroll_by) from
// `from_y`, the scroll position it is heading for (the end of a running animation, not where it is drawn: else a key
// pressed mid-animation drops what is left of the previous one, #34). Clamped to 0..max_y.
int32_t scroll_step_target(int32_t from_y, int32_t dy, int32_t max_y);
