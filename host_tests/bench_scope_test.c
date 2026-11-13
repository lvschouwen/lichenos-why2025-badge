// Host test: the bench console scope cell (#377): nothing bound is no scope (fail closed), else the bound check decides
#include "bench_scope.h"

#include <stdio.h>

static int  failures;
static bool scope_now;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool fake_scope(void) {
    return scope_now;
}

int main(void) {
    CHECK(!bench_scope_set()); // MeshCore not started
    bench_scope_bind(fake_scope);
    CHECK(!bench_scope_set());
    scope_now = true;
    CHECK(bench_scope_set());
    bench_scope_bind(NULL);
    CHECK(!bench_scope_set());
    if (failures) {
        printf("bench_scope_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("bench_scope_test: ok\n");
    return 0;
}
