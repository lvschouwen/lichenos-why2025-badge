// Host test: where a full-screen draw runs (#259): never on a stack too small for LVGL's draw path
#include "draw_stack.h"

#include <stdatomic.h>
#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// A fake wait: the job's done flag shows up after done_after waits; counts waits and clears
typedef struct {
    atomic_bool *done;
    int          done_after, waits, clears, waits_at_clear;
} fake_t;

static void fake_wait(void *ctx) {
    fake_t *f = ctx;
    if (++f->waits >= f->done_after) {
        atomic_store(f->done, true);
    }
}

static void fake_clear(void *ctx) {
    fake_t *f = ctx;
    f->clears++;
    f->waits_at_clear = f->waits;
}

static fake_t run_wait(bool done_at_start, int done_after) {
    atomic_bool done;
    atomic_init(&done, done_at_start);
    fake_t                f   = {.done = &done, .done_after = done_after};
    draw_wait_ops_t const ops = {.wait = fake_wait, .clear = fake_clear, .ctx = &f};
    draw_wait_done(&done, &ops);
    f.done = NULL;
    return f;
}

static void wait_tests(void) {
    // #259 review: two early wake-ups (ui_probe's late gives) do not end the wait; it ends only when the job is done
    fake_t f = run_wait(false, 3);
    CHECK(f.waits == 3);
    // the job's own give, still pending once done shows up, is dropped after the wait and never before it
    CHECK(f.clears == 1 && f.waits_at_clear == 3);
    // done before the first wait (the LVGL task was quick): no wait at all, still one clear
    f = run_wait(true, 1);
    CHECK(f.waits == 0 && f.clears == 1);
}

int main(void) {
    wait_tests();
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
