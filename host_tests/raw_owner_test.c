// Host test: who holds the raw keys and when the screen counts as the Keyboard test (#534)
#include "raw_owner.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_keytest_marks(void) {
    raw_owner_t o = {0};
    CHECK(!raw_owner_held(&o) && !raw_owner_test_mark(&o));
    CHECK(raw_owner_take(&o, 5, true));
    CHECK(raw_owner_held(&o) && raw_owner_test_mark(&o));
    CHECK(raw_owner_release_ctx(&o, 5));
    CHECK(!raw_owner_held(&o) && !raw_owner_test_mark(&o));
}

static void test_game_holds_without_mark(void) {
    raw_owner_t o = {0};
    CHECK(raw_owner_take(&o, 9, false));
    CHECK(raw_owner_held(&o));
    CHECK(!raw_owner_test_mark(&o)); // not the Keyboard test: nothing reads it as such
}

static void test_one_holder(void) {
    raw_owner_t o = {0};
    CHECK(raw_owner_take(&o, 9, false));
    CHECK(!raw_owner_take(&o, 5, true)); // a second taker is refused and does not set the mark
    CHECK(!raw_owner_test_mark(&o));
    CHECK(!raw_owner_take(&o, 9, false));
}

static void test_release_ctx(void) {
    raw_owner_t o = {0};
    CHECK(!raw_owner_release_ctx(&o, 9)); // nobody holds: nothing to do
    CHECK(raw_owner_take(&o, 9, false));
    CHECK(!raw_owner_release_ctx(&o, 5)); // another ctx's unload leaves the holder alone
    CHECK(raw_owner_held(&o));
    CHECK(raw_owner_release_ctx(&o, 9));
    CHECK(!raw_owner_held(&o));
    CHECK(!raw_owner_release_ctx(&o, 9)); // twice is harmless
    CHECK(raw_owner_take(&o, 5, true));   // free again, and the mark follows the new holder only
    CHECK(raw_owner_test_mark(&o));
}

static void test_no_ctx(void) {
    raw_owner_t o = {0};
    CHECK(!raw_owner_take(&o, 0, true));
    CHECK(!raw_owner_held(&o));
    CHECK(!raw_owner_release_ctx(&o, 0));
}

int main(void) {
    test_keytest_marks();
    test_game_holds_without_mark();
    test_one_holder();
    test_release_ctx();
    test_no_ctx();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("raw_owner_test ok\n");
    return 0;
}
