#include "restart_frame.h"

restart_frame_t restart_frame_plan(size_t psram_largest, size_t stack_free, bool on_ui_task) {
    if (psram_largest < RESTART_FRAME_HEAP_NEED) {
        return RESTART_FRAME_SKIP;
    }
    if (stack_free >= RESTART_FRAME_STACK_NEED) {
        return RESTART_FRAME_HERE;
    }
    return on_ui_task ? RESTART_FRAME_SKIP : RESTART_FRAME_UI_TASK;
}
