#include "slider_map.h"

static int clamp(int min, int max, int v) {
    return v < min ? min : v > max ? max : v;
}

int slider_map_pos(int min, int max, int value) {
    return clamp(min, max, value) - min;
}

int slider_map_value(int min, int max, int pos) {
    return clamp(min, max, min + pos);
}

int slider_map_step(int min, int max, int steps) {
    int step = steps > 0 ? (max - min) / steps : 1;
    return step < 1 ? 1 : step;
}
