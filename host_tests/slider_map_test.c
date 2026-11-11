#include "slider_map.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void) {
    // Display > Screen, 10..80: the lowest value sits at position 0, like Keyboard's 0
    CHECK(slider_map_pos(10, 80, 10) == 0);
    CHECK(slider_map_pos(10, 80, 45) == 35);
    CHECK(slider_map_pos(10, 80, 80) == 70);
    CHECK(slider_map_pos(10, 80, 3) == 0);   // a saved level below the floor
    CHECK(slider_map_pos(10, 80, 200) == 70);
    CHECK(slider_map_pos(0, 80, 0) == 0);
    CHECK(slider_map_pos(-9, 22, -9) == 0);
    CHECK(slider_map_value(10, 80, 0) == 10);
    CHECK(slider_map_value(10, 80, 35) == 45);
    CHECK(slider_map_value(10, 80, 70) == 80);
    CHECK(slider_map_value(10, 80, 99) == 80);
    CHECK(slider_map_value(10, 80, -5) == 10);
    CHECK(slider_map_step(10, 80, 20) == 3);
    CHECK(slider_map_step(0, 80, 20) == 4);
    CHECK(slider_map_step(0, 10, 20) == 1);
    CHECK(slider_map_step(0, 80, 0) == 1);
    if (failures) {
        return 1;
    }
    printf("slider_map_test: ok\n");
    return 0;
}
