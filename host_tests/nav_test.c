// Host test: the shell's screen stack, Esc back to home (#66)
#include "nav.h"

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
    nav_reset();
    CHECK(nav_current() == NAV_HOME);
    CHECK(nav_depth() == 1);

    // A repeated Enter asks for the same screen twice: the second push is refused and changes nothing
    CHECK(nav_push(3));
    CHECK(!nav_push(3));
    CHECK(nav_depth() == 2);
    CHECK(nav_push(4)); // another screen on top is fine
    CHECK(nav_back() == 3);
    CHECK(nav_back() == NAV_HOME);
    CHECK(nav_push(3)); // and the same one again once it was left
    nav_reset();

    CHECK(nav_push(1));
    CHECK(nav_push(2));
    CHECK(nav_current() == 2);
    CHECK(nav_depth() == 3);

    CHECK(nav_back() == 1);
    CHECK(nav_current() == 1);
    CHECK(nav_back() == NAV_HOME);
    CHECK(nav_depth() == 1);
    // Esc at home stays home
    CHECK(nav_back() == NAV_HOME);
    CHECK(nav_current() == NAV_HOME);
    CHECK(nav_depth() == 1);

    // Home plus 7 screens fills the stack; a 9th entry is refused and changes nothing
    for (int id = 1; id <= 7; id++) {
        CHECK(nav_push(id));
    }
    CHECK(nav_depth() == NAV_MAX_DEPTH);
    CHECK(!nav_push(99));
    CHECK(nav_depth() == NAV_MAX_DEPTH);
    CHECK(nav_current() == 7);

    nav_reset();
    CHECK(nav_current() == NAV_HOME);
    CHECK(nav_depth() == 1);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("nav_test: all passed\n");
    return 0;
}
