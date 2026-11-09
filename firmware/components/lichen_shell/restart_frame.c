#include "restart_frame.h"

restart_frame_t restart_frame_plan(size_t psram_largest, size_t stack_free) {
    if (psram_largest < RESTART_FRAME_HEAP_NEED) {
        return RESTART_FRAME_SKIP;
    }
    return stack_free >= RESTART_FRAME_STACK_NEED ? RESTART_FRAME_HERE : RESTART_FRAME_UI_TASK;
}
