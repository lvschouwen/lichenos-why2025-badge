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

static bool fake_chat(char *name, size_t cap, bool *dm, bool *selecting) {
    snprintf(name, cap, "%s", "#badgetest-but-longer");
    *dm        = false;
    *selecting = true;
    return true;
}

static bool no_chat(char *name, size_t cap, bool *dm, bool *selecting) {
    (void)name;
    (void)cap;
    *dm        = true; // a binder that leaves junk on false: the cell clears it
    *selecting = true;
    return false;
}

static bool starts_now;

static bool fake_starts(int top_screen, char const *focus) {
    return starts_now && top_screen == 0 && focus && strcmp(focus, "MeshCore") == 0;
}

static void test_starts(void) {
    CHECK(!bench_enter_starts(0, "MeshCore")); // unbound: MeshCore not registered
    bench_starts_bind(fake_starts);
    CHECK(!bench_enter_starts(0, "MeshCore"));
    starts_now = true;
    CHECK(bench_enter_starts(0, "MeshCore")); // the arguments reach the bound check
    CHECK(!bench_enter_starts(0, "Settings"));
    CHECK(!bench_enter_starts(3, "MeshCore"));
    bench_starts_bind(NULL);
    CHECK(!bench_enter_starts(0, "MeshCore"));
}

static void test_chat(void) {
    char name[11];
    bool dm = true, sel = true;
    CHECK(!bench_chat_on_top(name, sizeof(name), &dm, &sel) && name[0] == '\0' && !dm && !sel); // unbound
    bench_chat_bind(fake_chat);
    CHECK(bench_chat_on_top(name, sizeof(name), &dm, &sel) && strcmp(name, "#badgetest") == 0 && !dm && sel); // cut, terminated
    bench_chat_bind(no_chat);
    CHECK(!bench_chat_on_top(name, sizeof(name), &dm, &sel) && name[0] == '\0' && !dm && !sel);
    bench_chat_bind(NULL);
}

static bool screen_now;

static bool fake_screen(void) {
    return screen_now;
}

static void test_screen_scoped(void) {
    CHECK(!bench_screen_scoped()); // unbound: fail closed
    bench_screen_scoped_bind(fake_screen);
    CHECK(!bench_screen_scoped());
    screen_now = true;
    CHECK(bench_screen_scoped());
    bench_screen_scoped_bind(NULL);
    CHECK(!bench_screen_scoped());
}

int main(void) {
    test_screen_scoped();
    CHECK(!bench_scope_set()); // MeshCore not started
    bench_scope_bind(fake_scope);
    CHECK(!bench_scope_set());
    scope_now = true;
    CHECK(bench_scope_set());
    bench_scope_bind(NULL);
    CHECK(!bench_scope_set());
    test_chat();
    test_starts();
    if (failures) {
        printf("bench_scope_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("bench_scope_test: ok\n");
    return 0;
}
