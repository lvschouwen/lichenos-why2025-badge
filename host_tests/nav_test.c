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

// The test drives the stack with key 0 (the keyless screens)
static bool nav_push(int screen_id) {
    return nav_push_keyed(screen_id, 0);
}

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

    // Identity is (id, key) (#148): the same id with another key is another screen
    int frame;
    nav_reset();
    CHECK(nav_decide(5, 0, &frame) == NAV_OPEN_NEW && frame == -1);
    CHECK(nav_push_keyed(5, 7));
    CHECK(nav_push_keyed(6, 0));
    CHECK(nav_decide(6, 0, &frame) == NAV_OPEN_KEEP && frame == 2); // on top
    CHECK(nav_decide(5, 7, &frame) == NAV_OPEN_BACK_TO && frame == 1); // lower: go back, not a second copy
    CHECK(nav_decide(5, 8, &frame) == NAV_OPEN_NEW);                   // another chat of the same screen id
    CHECK(nav_decide(5, 0, &frame) == NAV_OPEN_NEW);
    CHECK(!nav_push_keyed(5, 7)); // a second copy is refused by the stack itself
    CHECK(nav_depth() == 3);
    CHECK(nav_push_keyed(5, 8)); // different keys stack
    CHECK(nav_key_at(1) == 7 && nav_key_at(3) == 8 && nav_key_at(4) == 0);
    CHECK(nav_decide(NAV_HOME, 0, &frame) == NAV_OPEN_BACK_TO && frame == 0);
    CHECK(nav_back() == 6);
    CHECK(nav_decide(5, 8, &frame) == NAV_OPEN_NEW); // left: opens again
    nav_reset();
    CHECK(nav_push_keyed(9, NAV_KEY_UNIQUE));
    CHECK(nav_decide(9, NAV_KEY_UNIQUE, &frame) == NAV_OPEN_NEW); // "always new" never matches
    CHECK(nav_push_keyed(9, NAV_KEY_UNIQUE));
    CHECK(nav_decide(9, 0, &frame) == NAV_OPEN_NEW);
    // The keys themselves come from the gate's shell_key_hash (ui_ops_test.c, #221)

    // A full stack still answers for the screens on it: the bottom one goes BACK_TO, a new one is refused
    nav_reset();
    for (int id = 1; id < NAV_MAX_DEPTH; id++) {
        CHECK(nav_push_keyed(id, (uint32_t)id));
    }
    CHECK(nav_depth() == NAV_MAX_DEPTH);
    CHECK(nav_decide(1, 1, &frame) == NAV_OPEN_BACK_TO && frame == 1);
    CHECK(nav_decide(NAV_MAX_DEPTH - 1, NAV_MAX_DEPTH - 1, &frame) == NAV_OPEN_KEEP);
    CHECK(nav_decide(50, 0, &frame) == NAV_OPEN_NEW);
    CHECK(!nav_push_keyed(50, 0) && nav_depth() == NAV_MAX_DEPTH);
    nav_reset();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("nav_test: all passed\n");
    return 0;
}
