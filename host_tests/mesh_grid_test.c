// Host test: the MeshCore app's home grid (#88): tiles, badges, and keys that move exactly as on Lichen's home
#include "home_layout.h"
#include "mesh_grid.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char out[MESH_GRID_BADGE_MAX];

static char const *badge(mesh_grid_tile_t tile, mesh_grid_counts_t const *c) {
    mesh_grid_badge(tile, c, out, sizeof(out));
    return out;
}

static void test_tiles(void) {
    CHECK(MESH_GRID_TILES == 6);
    CHECK(strcmp(mesh_grid_name(MESH_GRID_CHAT), "Chat") == 0);
    CHECK(strcmp(mesh_grid_name(MESH_GRID_CONTACTS), "Contacts") == 0);
    CHECK(strcmp(mesh_grid_name(MESH_GRID_REPEATERS), "Repeaters") == 0);
    CHECK(strcmp(mesh_grid_name(MESH_GRID_HEARD), "Heard") == 0);
    CHECK(strcmp(mesh_grid_name(MESH_GRID_ADVERTISE), "Advertise") == 0);
    CHECK(strcmp(mesh_grid_name(MESH_GRID_SETTINGS), "Settings") == 0);
    CHECK(strcmp(mesh_grid_name(MESH_GRID_TILES), "") == 0);
    CHECK(strcmp(mesh_grid_name((mesh_grid_tile_t)-1), "") == 0);
    CHECK(MESH_GRID_COLS == HOME_GRID_COLS);
}

static void test_counts(void) {
    mesh_grid_count_text(0, out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    mesh_grid_count_text(1, out, sizeof(out));
    CHECK(strcmp(out, "1") == 0);
    mesh_grid_count_text(99, out, sizeof(out));
    CHECK(strcmp(out, "99") == 0);
    mesh_grid_count_text(100, out, sizeof(out));
    CHECK(strcmp(out, "99+") == 0);
    mesh_grid_count_text(1000, out, sizeof(out));
    CHECK(strcmp(out, "99+") == 0);
    char tiny[3];
    mesh_grid_count_text(100, tiny, sizeof(tiny)); // cut, still terminated
    CHECK(strcmp(tiny, "99") == 0);
}

static void test_ages(void) {
    mesh_grid_age_text(0, out, sizeof(out));
    CHECK(strcmp(out, "now") == 0);
    mesh_grid_age_text(59, out, sizeof(out));
    CHECK(strcmp(out, "now") == 0);
    mesh_grid_age_text(60, out, sizeof(out));
    CHECK(strcmp(out, "1m") == 0);
    mesh_grid_age_text(3599, out, sizeof(out));
    CHECK(strcmp(out, "59m") == 0);
    mesh_grid_age_text(3600, out, sizeof(out));
    CHECK(strcmp(out, "1h") == 0);
    mesh_grid_age_text(86399, out, sizeof(out));
    CHECK(strcmp(out, "23h") == 0);
    mesh_grid_age_text(86400, out, sizeof(out));
    CHECK(strcmp(out, "1d") == 0);
    mesh_grid_age_text(99 * 86400 + 5, out, sizeof(out));
    CHECK(strcmp(out, "99d") == 0);
    mesh_grid_age_text(100 * 86400, out, sizeof(out));
    CHECK(strcmp(out, "99d+") == 0);
    mesh_grid_age_text(UINT32_MAX, out, sizeof(out));
    CHECK(strcmp(out, "99d+") == 0);
}

static void test_badges(void) {
    mesh_grid_counts_t c = {0};
    for (int t = 0; t < MESH_GRID_TILES; t++) {
        CHECK(strcmp(badge((mesh_grid_tile_t)t, &c), "") == 0); // nothing yet: no badge anywhere
    }
    c = (mesh_grid_counts_t){.unread       = 120,
                             .contacts     = 42,
                             .repeaters    = 3,
                             .heard_any    = true,
                             .heard_age_s  = 125,
                             .advert_sent  = true,
                             .advert_age_s = 7200};
    CHECK(strcmp(badge(MESH_GRID_CHAT, &c), "99+") == 0);
    CHECK(strcmp(badge(MESH_GRID_CONTACTS, &c), "42") == 0);
    CHECK(strcmp(badge(MESH_GRID_REPEATERS, &c), "3") == 0);
    CHECK(strcmp(badge(MESH_GRID_HEARD, &c), "2m") == 0);
    CHECK(strcmp(badge(MESH_GRID_ADVERTISE, &c), "2h") == 0);
    CHECK(strcmp(badge(MESH_GRID_SETTINGS, &c), "") == 0);
    c.unread = 5;
    CHECK(strcmp(badge(MESH_GRID_CHAT, &c), "5") == 0);
    c.advert_sent = false; // an age without a sent advert means nothing
    CHECK(strcmp(badge(MESH_GRID_ADVERTISE, &c), "") == 0);
    c.heard_any = false;
    CHECK(strcmp(badge(MESH_GRID_HEARD, &c), "") == 0);
    c.advert_sent  = true;
    c.advert_age_s = 0;
    CHECK(strcmp(badge(MESH_GRID_ADVERTISE, &c), "now") == 0);
}

// The six tiles: Chat Contacts Repeaters / Heard Advertise Settings
static void test_moves(void) {
    int n = MESH_GRID_TILES;
    CHECK(mesh_grid_move(n, MESH_GRID_CHAT, MESH_GRID_KEY_RIGHT) == MESH_GRID_CONTACTS);
    CHECK(mesh_grid_move(n, MESH_GRID_REPEATERS, MESH_GRID_KEY_RIGHT) == MESH_GRID_REPEATERS); // no wrap
    CHECK(mesh_grid_move(n, MESH_GRID_HEARD, MESH_GRID_KEY_LEFT) == MESH_GRID_HEARD);
    CHECK(mesh_grid_move(n, MESH_GRID_CHAT, MESH_GRID_KEY_DOWN) == MESH_GRID_HEARD);
    CHECK(mesh_grid_move(n, MESH_GRID_SETTINGS, MESH_GRID_KEY_UP) == MESH_GRID_REPEATERS);
    CHECK(mesh_grid_move(n, MESH_GRID_ADVERTISE, MESH_GRID_KEY_DOWN) == MESH_GRID_ADVERTISE);
    CHECK(mesh_grid_move(n, MESH_GRID_CONTACTS, MESH_GRID_KEY_UP) == MESH_GRID_CONTACTS);
    CHECK(mesh_grid_move(n, MESH_GRID_CONTACTS, 10) == MESH_GRID_CONTACTS); // Enter is not a move
    CHECK(mesh_grid_move(n, 9, MESH_GRID_KEY_LEFT) == 0);
    CHECK(mesh_grid_move(0, 0, MESH_GRID_KEY_DOWN) == 0);
}

// Identical to Lichen's home grid for every size up to its largest table, every index and every key
static void test_same_as_home(void) {
    static uint32_t const keys[] = {MESH_GRID_KEY_UP, MESH_GRID_KEY_DOWN, MESH_GRID_KEY_LEFT, MESH_GRID_KEY_RIGHT, 10, 27};
    CHECK(MESH_GRID_KEY_UP == HOME_KEY_UP && MESH_GRID_KEY_DOWN == HOME_KEY_DOWN);
    CHECK(MESH_GRID_KEY_LEFT == HOME_KEY_LEFT && MESH_GRID_KEY_RIGHT == HOME_KEY_RIGHT);
    for (int count = 0; count <= 16; count++) {
        for (int index = -1; index <= count; index++) {
            for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
                CHECK(mesh_grid_move(count, index, keys[k]) == home_move(HOME_GRID, count, index, keys[k]));
            }
        }
    }
}

int main(void) {
    test_tiles();
    test_counts();
    test_ages();
    test_badges();
    test_moves();
    test_same_as_home();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_grid_test: all passed\n");
    return 0;
}
