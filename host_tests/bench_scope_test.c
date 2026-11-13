// Host test: the bench console scope cell (#377): nothing bound is no scope (fail closed), else the bound check decides
#include "bench_scope.h"

#include <stdio.h>
#include <string.h>

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

static bool fake_chat(char *name, size_t cap, bool *dm) {
    snprintf(name, cap, "%s", "#badgetest-but-longer");
    *dm = false;
    return true;
}

static bool no_chat(char *name, size_t cap, bool *dm) {
    (void)name;
    (void)cap;
    *dm = true; // a binder that leaves junk on false: the cell clears it
    return false;
}

static void test_chat(void) {
    char name[11];
    bool dm = true;
    CHECK(!bench_chat_on_top(name, sizeof(name), &dm) && name[0] == '\0' && !dm); // unbound
    bench_chat_bind(fake_chat);
    CHECK(bench_chat_on_top(name, sizeof(name), &dm) && strcmp(name, "#badgetest") == 0 && !dm); // cut, terminated
    bench_chat_bind(no_chat);
    CHECK(!bench_chat_on_top(name, sizeof(name), &dm) && name[0] == '\0' && !dm);
    bench_chat_bind(NULL);
}

int main(void) {
    CHECK(!bench_scope_set()); // MeshCore not started
    bench_scope_bind(fake_scope);
    CHECK(!bench_scope_set());
    scope_now = true;
    CHECK(bench_scope_set());
    bench_scope_bind(NULL);
    CHECK(!bench_scope_set());
    test_chat();
    if (failures) {
        printf("bench_scope_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("bench_scope_test: ok\n");
    return 0;
}
