#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Home's two layouts and how the arrow keys move the selection in them (#66). Pure, host-tested in
// host_tests/home_layout_test.c.

typedef enum { HOME_TILES = 0, HOME_GRID = 1 } home_layout_t;

#define HOME_GRID_COLS 3

// The key values LVGL's keypad sends (LV_KEY_*); home.c static_asserts them against lvgl.h
#define HOME_KEY_UP    17
#define HOME_KEY_DOWN  18
#define HOME_KEY_RIGHT 19
#define HOME_KEY_LEFT  20

// Anything but 0 or 1 (a stale or corrupt NVS value) means tiles
home_layout_t home_layout_from_u8(uint8_t value);

// The new selection after `key` (other keys and moves with nothing there keep `index`). Tiles: Up/Down, clamped. Grid:
// three columns, arrows move in 2-D and stop at the edges and where there is no app.
int home_move(home_layout_t layout, int count, int index, uint32_t key);

// The count bubble on an app's icon (#102): false and "" for 0 (no bubble), "1".."99", "99+" above that
#define HOME_BADGE_CAP 99
bool home_badge_text(unsigned count, char *out, size_t cap);
