#include "psram_rotation.h"

int psram_rot_next(bool const used[], int n, int after) {
    for (int step = 1; step <= n; step++) {
        int i = ((after + step) % n + n) % n;
        if (used[i]) {
            return i;
        }
    }
    return -1;
}
