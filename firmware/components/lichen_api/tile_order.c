// A grid's display order (#189): parse, merge, move, encode. Pure; see tile_order.h.
#include "tile_order.h"

#include <string.h>

// The index of the id that is exactly text[0..len), -1 when none is
static int find_id(char const *text, size_t len, char const *const *ids, int n) {
    for (int i = 0; i < n; i++) {
        if (ids[i] && strlen(ids[i]) == len && memcmp(ids[i], text, len) == 0) {
            return i;
        }
    }
    return -1;
}

// Printable ASCII only: anything else is a corrupt NVS value, not a list with an odd id in it
static bool printable(char const *text) {
    for (; *text; text++) {
        if ((unsigned char)*text < 0x20 || (unsigned char)*text > 0x7E) {
            return false;
        }
    }
    return true;
}

bool tile_order_parse(char const *saved, char const *const *ids, int n, int *found, int *count) {
    *count = 0;
    if (!saved || saved[0] != TILE_ORDER_VERSION || saved[1] != ':' || !printable(saved)) {
        return false;
    }
    for (char const *at = saved + 2; *at;) {
        char const *end = strchr(at, ',');
        size_t      len = end ? (size_t)(end - at) : strlen(at);
        int         idx = len ? find_id(at, len, ids, n) : -1;
        bool        dup = false;
        for (int i = 0; idx >= 0 && i < *count; i++) {
            dup = dup || found[i] == idx;
        }
        if (idx >= 0 && !dup) {
            found[(*count)++] = idx; // at most n: every index once
        }
        at += len + (end ? 1 : 0);
    }
    return true;
}

bool tile_order_merge(char const *saved, char const *const *ids, int n, int *order) {
    int  count = 0;
    bool used  = tile_order_parse(saved, ids, n, order, &count);
    // What saved did not name follows in registration order: a new app goes to the end
    for (int i = 0; i < n; i++) {
        bool placed = false;
        for (int s = 0; s < count && !placed; s++) {
            placed = order[s] == i;
        }
        if (!placed) {
            order[count++] = i;
        }
    }
    return used;
}

size_t tile_order_encode(char const *const *ids, int const *order, int n, char *out, size_t cap) {
    if (cap < 3) {
        if (cap) {
            out[0] = '\0';
        }
        return 0;
    }
    out[0]     = TILE_ORDER_VERSION;
    out[1]     = ':';
    size_t len = 2;
    for (int s = 0; s < n; s++) {
        char const *id = order[s] >= 0 && order[s] < n ? ids[order[s]] : NULL;
        size_t      il = id ? strlen(id) : 0;
        size_t      need = il + (s ? 1 : 0);
        if (!il || strpbrk(id, ",:") || len + need + 1 > cap) {
            out[0] = '\0';
            return 0;
        }
        if (s) {
            out[len++] = ',';
        }
        memcpy(out + len, id, il);
        len += il;
    }
    out[len] = '\0';
    return len;
}

bool tile_order_move(int *order, int n, int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= n || to >= n) {
        return false;
    }
    int moving = order[from];
    if (from < to) {
        memmove(order + from, order + from + 1, (size_t)(to - from) * sizeof(*order));
    } else {
        memmove(order + to + 1, order + to, (size_t)(from - to) * sizeof(*order));
    }
    order[to] = moving;
    return true;
}

bool tile_order_is_default(int const *order, int n) {
    for (int i = 0; i < n; i++) {
        if (order[i] != i) {
            return false;
        }
    }
    return true;
}

void tile_order_slots(int const *order, int n, int *slot_of) {
    for (int s = 0; s < n; s++) {
        slot_of[order[s]] = s;
    }
}

tile_cell_t tile_cell_size(int avail_w, int cols, int line_h) {
    tile_cell_t c = {0};
    if (cols <= 0) {
        return c;
    }
    c.w          = (avail_w - (cols - 1) * TILE_GRID_COL_GAP) / cols;
    c.pad_bottom = line_h + TILE_GRID_MOVING_EDGE + 2;
    c.h = 2 * TILE_GRID_BORDER_W + TILE_GRID_PAD_TOP + TILE_GRID_ICON_PX + TILE_GRID_ICON_GAP + line_h + c.pad_bottom;
    return c;
}
