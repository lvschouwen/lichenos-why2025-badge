// Host test: where the "Restarting" frame is drawn (#255): never on a stack too small for LVGL's draw path
#include "restart_frame.h"

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
    size_t const heap = 4u << 20;
    // #255: fw psram speed 120 yes on the 4 KiB console task, ~2 KiB of it left at the restart: the LVGL task draws
    CHECK(restart_frame_plan(heap, 2048) == RESTART_FRAME_UI_TASK);
    CHECK(restart_frame_plan(heap, 4096) == RESTART_FRAME_UI_TASK); // even a whole console stack is too small
    CHECK(restart_frame_plan(heap, RESTART_FRAME_STACK_NEED - 1) == RESTART_FRAME_UI_TASK);
    CHECK(restart_frame_plan(heap, 0) == RESTART_FRAME_UI_TASK);
    // the LVGL task itself (a Settings or Power button) or another big stack: drawn right away, as before
    CHECK(restart_frame_plan(heap, RESTART_FRAME_STACK_NEED) == RESTART_FRAME_HERE);
    CHECK(restart_frame_plan(heap, 8 * 1024) == RESTART_FRAME_HERE);
    // out of PSRAM: no frame wherever it would be drawn (the frame allocates)
    CHECK(restart_frame_plan(RESTART_FRAME_HEAP_NEED - 1, 8 * 1024) == RESTART_FRAME_SKIP);
    CHECK(restart_frame_plan(RESTART_FRAME_HEAP_NEED - 1, 1024) == RESTART_FRAME_SKIP);
    CHECK(restart_frame_plan(0, 0) == RESTART_FRAME_SKIP);
    CHECK(restart_frame_plan(RESTART_FRAME_HEAP_NEED, 1024) == RESTART_FRAME_UI_TASK);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("restart_frame_test: all passed\n");
    return 0;
}
