// Host test: the rules of the @ID@ app (scaffolded by `tools/badge.sh new-app`). Add a case for every rule you add.
#include "@ID@_model.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_init(void) {
    @ID@_model_t m;
    @ID@_model_init(&m, 7, 5);
    CHECK(m.count == 7 && m.step == 5);
    @ID@_model_init(&m, @UP@_COUNT_MAX + 50, 10);
    CHECK(m.count == @UP@_COUNT_MAX && m.step == 10);
    @ID@_model_init(&m, 0, 3);
    CHECK(m.step == 1); // a saved step nobody offers is dropped
}

static void test_tap(void) {
    @ID@_model_t m;
    @ID@_model_init(&m, 0, 5);
    CHECK(@ID@_model_tap(&m) && m.count == 5);
    @ID@_model_init(&m, @UP@_COUNT_MAX - 3, 10);
    CHECK(@ID@_model_tap(&m) && m.count == @UP@_COUNT_MAX); // the last tap stops at the limit, it does not pass it
    CHECK(!@ID@_model_tap(&m) && m.count == @UP@_COUNT_MAX);
}

static void test_step_cycle(void) {
    CHECK(@ID@_model_next_step(1) == 5);
    CHECK(@ID@_model_next_step(5) == 10);
    CHECK(@ID@_model_next_step(10) == 1);
    CHECK(@ID@_model_next_step(7) == 1);
}

static void test_text(void) {
    @ID@_model_t m;
    char         out[16], tiny[4];
    @ID@_model_init(&m, 0, 1);
    @ID@_model_text(&m, out, sizeof(out));
    CHECK(strcmp(out, "no taps") == 0);
    @ID@_model_init(&m, 1, 1);
    @ID@_model_text(&m, out, sizeof(out));
    CHECK(strcmp(out, "1 tap") == 0);
    @ID@_model_init(&m, 42, 1);
    @ID@_model_text(&m, out, sizeof(out));
    CHECK(strcmp(out, "42 taps") == 0);
    @ID@_model_text(&m, tiny, sizeof(tiny)); // too small: cut, still terminated
    CHECK(strlen(tiny) == 3);
    @ID@_model_text(&m, tiny, 0);            // nothing written, nothing crashes
}

int main(void) {
    test_init();
    test_tap();
    test_step_cycle();
    test_text();
    if (failures) {
        printf("@ID@_model_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("@ID@_model_test: ok\n");
    return 0;
}
