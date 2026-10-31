// Host test: home's key navigation (#66)
#include "home_layout.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    // Tiles, 3 apps
    CHECK(home_move(HOME_TILES, 3, 2, HOME_KEY_DOWN) == 2);
    CHECK(home_move(HOME_TILES, 3, 0, HOME_KEY_UP) == 0);
    CHECK(home_move(HOME_TILES, 3, 0, HOME_KEY_DOWN) == 1);
    CHECK(home_move(HOME_TILES, 3, 2, HOME_KEY_UP) == 1);
    CHECK(home_move(HOME_TILES, 3, 1, HOME_KEY_LEFT) == 1);
    CHECK(home_move(HOME_TILES, 3, 1, 'a') == 1);

    // Grid, 5 apps: row 0 = 0 1 2, row 1 = 3 4
    CHECK(home_move(HOME_GRID, 5, 2, HOME_KEY_RIGHT) == 2);
    CHECK(home_move(HOME_GRID, 5, 1, HOME_KEY_DOWN) == 4);
    CHECK(home_move(HOME_GRID, 5, 2, HOME_KEY_DOWN) == 2);
    CHECK(home_move(HOME_GRID, 5, 3, HOME_KEY_LEFT) == 3);
    CHECK(home_move(HOME_GRID, 5, 0, HOME_KEY_RIGHT) == 1);
    CHECK(home_move(HOME_GRID, 5, 4, HOME_KEY_RIGHT) == 4); // no app at 5
    CHECK(home_move(HOME_GRID, 5, 4, HOME_KEY_UP) == 1);
    CHECK(home_move(HOME_GRID, 5, 1, HOME_KEY_UP) == 1);
    CHECK(home_move(HOME_GRID, 5, 4, HOME_KEY_LEFT) == 3);

    // More apps than the old table of 8 (LICHEN_MAX_APPS is 16 now): 13 apps, tiles walk to the end and back
    int at = 0;
    for (int i = 0; i < 20; i++) {
        at = home_move(HOME_TILES, 13, at, HOME_KEY_DOWN);
    }
    CHECK(at == 12);
    for (int i = 0; i < 20; i++) {
        at = home_move(HOME_TILES, 13, at, HOME_KEY_UP);
    }
    CHECK(at == 0);
    // Grid, 13 apps: rows 0 1 2 / 3 4 5 / 6 7 8 / 9 10 11 / 12
    CHECK(home_move(HOME_GRID, 13, 8, HOME_KEY_DOWN) == 11);
    CHECK(home_move(HOME_GRID, 13, 11, HOME_KEY_DOWN) == 11); // 14 would be past the last app
    CHECK(home_move(HOME_GRID, 13, 9, HOME_KEY_DOWN) == 12);
    CHECK(home_move(HOME_GRID, 13, 12, HOME_KEY_RIGHT) == 12);
    CHECK(home_move(HOME_GRID, 13, 12, HOME_KEY_UP) == 9);
    CHECK(home_move(HOME_GRID, 13, 10, HOME_KEY_RIGHT) == 11);
    // 16 apps, the largest table: rows of 3, the last row holds only app 15
    at = 0;
    for (int i = 0; i < 8; i++) {
        at = home_move(HOME_GRID, 16, at, HOME_KEY_DOWN);
    }
    CHECK(at == 15);
    CHECK(home_move(HOME_GRID, 16, 15, HOME_KEY_RIGHT) == 15);
    CHECK(home_move(HOME_GRID, 16, 15, HOME_KEY_UP) == 12);
    CHECK(home_move(HOME_TILES, 16, 15, HOME_KEY_DOWN) == 15);

    // Nothing to move on
    CHECK(home_move(HOME_TILES, 0, 0, HOME_KEY_DOWN) == 0);
    CHECK(home_move(HOME_GRID, 0, 0, HOME_KEY_RIGHT) == 0);

    CHECK(home_layout_from_u8(0) == HOME_TILES);
    CHECK(home_layout_from_u8(1) == HOME_GRID);
    CHECK(home_layout_from_u8(200) == HOME_TILES);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("home_layout_test: ok\n");
    return 0;
}
