#include "draw_stack.h"

draw_stack_t draw_stack_plan(size_t stack_free, bool on_ui_task, bool ui_task_known) {
    if (stack_free >= DRAW_STACK_NEED) {
        return DRAW_HERE;
    }
    if (on_ui_task || !ui_task_known) {
        return DRAW_REFUSE; // the LVGL task short of stack cannot hand the draw to itself and wait
    }
    return DRAW_UI_TASK;
}
