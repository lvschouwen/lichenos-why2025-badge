#include "home_layout.h"

#include <stdio.h>

home_layout_t home_layout_from_u8(uint8_t value) {
    return value == HOME_GRID ? HOME_GRID : HOME_TILES;
}

static int move_tiles(int count, int index, uint32_t key) {
    if (key == HOME_KEY_UP && index > 0) {
        return index - 1;
    }
    if (key == HOME_KEY_DOWN && index + 1 < count) {
        return index + 1;
    }
    return index;
}

static int move_grid(int count, int index, uint32_t key) {
    int col = index % HOME_GRID_COLS;
    switch (key) {
        case HOME_KEY_LEFT: return col > 0 ? index - 1 : index;
        case HOME_KEY_RIGHT: return col < HOME_GRID_COLS - 1 && index + 1 < count ? index + 1 : index;
        case HOME_KEY_UP: return index >= HOME_GRID_COLS ? index - HOME_GRID_COLS : index;
        case HOME_KEY_DOWN: return index + HOME_GRID_COLS < count ? index + HOME_GRID_COLS : index;
        default: return index;
    }
}

int home_move(home_layout_t layout, int count, int index, uint32_t key) {
    if (count <= 0 || index < 0 || index >= count) {
        return 0;
    }
    return layout == HOME_GRID ? move_grid(count, index, key) : move_tiles(count, index, key);
}

bool home_badge_text(unsigned count, char *out, size_t cap) {
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (count == 0) {
        return false;
    }
    if (count > HOME_BADGE_CAP) {
        snprintf(out, cap, "%u+", (unsigned)HOME_BADGE_CAP);
    } else {
        snprintf(out, cap, "%u", count);
    }
    return true;
}
