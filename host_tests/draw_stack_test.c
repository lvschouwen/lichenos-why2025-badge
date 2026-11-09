// Host test: where a full-screen draw runs (#259): never on a stack too small for LVGL's draw path
#include "draw_stack.h"

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
    // #259: "fw ui perf bench" on the 4 KiB console task drew right there; it goes to the LVGL task now
    CHECK(draw_stack_plan(3000, false, true) == DRAW_UI_TASK);
    CHECK(draw_stack_plan(4096, false, true) == DRAW_UI_TASK); // even a whole console stack is too small
    CHECK(draw_stack_plan(DRAW_STACK_NEED - 1, false, true) == DRAW_UI_TASK);
    CHECK(draw_stack_plan(0, false, true) == DRAW_UI_TASK);
    // enough free stack (app_main's boot screen, the LVGL task in the Bench app's timer): drawn on the caller
    CHECK(draw_stack_plan(DRAW_STACK_NEED, false, true) == DRAW_HERE);
    CHECK(draw_stack_plan(DRAW_STACK_NEED, true, true) == DRAW_HERE);
    CHECK(draw_stack_plan(8 * 1024, false, false) == DRAW_HERE);
    // the LVGL task short of stack: it cannot wait for itself
    CHECK(draw_stack_plan(DRAW_STACK_NEED - 1, true, true) == DRAW_REFUSE);
    // before shell_start noted the LVGL task: no hand-over into the unknown, a plain refusal
    CHECK(draw_stack_plan(3000, false, false) == DRAW_REFUSE);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("draw_stack_test: all passed\n");
    return 0;
}
