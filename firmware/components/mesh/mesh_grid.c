// The MeshCore app's home grid model (#88): see mesh_grid.h
#include "mesh_grid.h"

#include <stdio.h>

static char const *const NAMES[MESH_GRID_TILES] = {"Chat", "Contacts", "Repeaters", "Heard", "Advertise", "Settings"};

char const *mesh_grid_name(mesh_grid_tile_t tile) {
    return (unsigned)tile < MESH_GRID_TILES ? NAMES[tile] : "";
}

void mesh_grid_count_text(size_t n, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (n == 0) {
        out[0] = '\0';
    } else if (n > MESH_GRID_BADGE_CAP) {
        snprintf(out, cap, "%u+", (unsigned)MESH_GRID_BADGE_CAP);
    } else {
        snprintf(out, cap, "%u", (unsigned)n);
    }
}

// Short units: the badge sits in a corner of a tile a third of the screen wide
void mesh_grid_age_text(uint32_t age_s, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    uint32_t days = age_s / 86400;
    if (age_s < 60) {
        snprintf(out, cap, "now");
    } else if (age_s < 3600) {
        snprintf(out, cap, "%um", (unsigned)(age_s / 60));
    } else if (days == 0) {
        snprintf(out, cap, "%uh", (unsigned)(age_s / 3600));
    } else if (days <= 99) {
        snprintf(out, cap, "%ud", (unsigned)days);
    } else {
        snprintf(out, cap, "99d+");
    }
}

void mesh_grid_badge(mesh_grid_tile_t tile, mesh_grid_counts_t const *c, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    switch (tile) {
        case MESH_GRID_CHAT: mesh_grid_count_text(c->unread, out, cap); break;
        case MESH_GRID_CONTACTS: mesh_grid_count_text(c->contacts, out, cap); break;
        case MESH_GRID_REPEATERS: mesh_grid_count_text(c->repeaters, out, cap); break;
        case MESH_GRID_HEARD:
            if (c->heard_any) {
                mesh_grid_age_text(c->heard_age_s, out, cap);
            }
            break;
        case MESH_GRID_ADVERTISE:
            if (c->advert_sent) {
                mesh_grid_age_text(c->advert_age_s, out, cap);
            }
            break;
        default: break; // Settings: nothing to count
    }
}

// The same rules as home_layout.c's grid: mesh may not depend on lichen_shell (#84), so the host test holds the two
// to each other for every count, index and key
int mesh_grid_move(int count, int index, uint32_t key) {
    if (count <= 0 || index < 0 || index >= count) {
        return 0;
    }
    int col = index % MESH_GRID_COLS;
    switch (key) {
        case MESH_GRID_KEY_LEFT: return col > 0 ? index - 1 : index;
        case MESH_GRID_KEY_RIGHT: return col < MESH_GRID_COLS - 1 && index + 1 < count ? index + 1 : index;
        case MESH_GRID_KEY_UP: return index >= MESH_GRID_COLS ? index - MESH_GRID_COLS : index;
        case MESH_GRID_KEY_DOWN: return index + MESH_GRID_COLS < count ? index + MESH_GRID_COLS : index;
        default: return index;
    }
}
