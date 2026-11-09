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

void draw_wait_done(atomic_bool const *done, draw_wait_ops_t const *ops) {
    while (!atomic_load_explicit(done, memory_order_acquire)) {
        ops->wait(ops->ctx);
    }
    ops->clear(ops->ctx);
}
