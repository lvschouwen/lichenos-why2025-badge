// Host test: the shell's notification texts and toast queue (#62)
#include "notify_view.h"

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

static nview_toast_t T(uint32_t id, uint8_t level) {
    nview_toast_t t = {id, level, false};
    return t;
}

static void test_queue(void) {
    nview_queue_t q;
    memset(&q, 0, sizeof(q));
    nview_toast_t out;
    CHECK(!nview_pop(&q, &out));
    // Same entry: replaced in place, highest level and any feedback kept
    CHECK(nview_push(&q, T(1, 0)) && nview_push(&q, T(2, 0)));
    nview_toast_t again = {1, 1, true};
    CHECK(nview_push(&q, again) && q.n == 2 && q.q[0].id == 1 && q.q[0].level == 1 && q.q[0].feedback);
    // Full: the oldest info goes, order otherwise kept
    CHECK(nview_push(&q, T(3, 2)) && nview_push(&q, T(4, 0)) && q.n == 4);
    CHECK(nview_push(&q, T(5, 1)) && q.n == 4);
    CHECK(q.q[0].id == 1 && q.q[1].id == 3 && q.q[2].id == 4 && q.q[3].id == 5); // 2 (oldest info) went
    // Full of warnings and errors: an info is dropped, a warning pushes out the oldest warning
    CHECK(nview_push(&q, T(6, 1)) && q.q[2].id == 5 && q.n == 4); // 4 (the only info) went
    CHECK(!nview_push(&q, T(7, 0)));
    CHECK(nview_push(&q, T(8, 1)) && q.q[0].id == 3); // 1 was the oldest warning
    // Pop in order; forget removes one
    nview_forget(&q, 6);
    CHECK(q.n == 3 && nview_pop(&q, &out) && out.id == 3 && nview_pop(&q, &out) && out.id == 5);
    nview_forget(&q, 99);
    CHECK(q.n == 1);
}

static void test_texts(void) {
    char out[64];
    CHECK(nview_toast_ms(0) == 3000 && nview_toast_ms(1) == 6000 && nview_toast_ms(2) == 10000);
    nview_title(out, sizeof(out), "#badgetest", 5);
    CHECK(strcmp(out, "#badgetest (\xC3\x97" "5)") == 0);
    nview_title(out, sizeof(out), "Clock answer ignored", 1);
    CHECK(strcmp(out, "Clock answer ignored") == 0);

    nview_age(out, sizeof(out), 3, 100, 0, 3, 130, 0);
    CHECK(strcmp(out, "now") == 0);
    nview_age(out, sizeof(out), 3, 100, 0, 3, 100 + 4 * 60 + 5, 0);
    CHECK(strcmp(out, "4 min") == 0);
    nview_age(out, sizeof(out), 3, 0, 0, 3, 7300, 0);
    CHECK(strcmp(out, "2 h") == 0);
    nview_age(out, sizeof(out), 2, 100, 0, 3, 50, 1800000000);
    CHECK(strcmp(out, "earlier boot") == 0); // no wall clock on the entry
    nview_age(out, sizeof(out), 2, 100, 1800000000 - 3 * 86400, 3, 50, 1800000000);
    CHECK(strcmp(out, "3 d") == 0); // across boots by the wall clock
    nview_age(out, sizeof(out), 2, 100, 1800000000, 3, 50, 0);
    CHECK(strcmp(out, "earlier boot") == 0); // clock not set now

    nview_count(out, sizeof(out), 0);
    CHECK(out[0] == '\0');
    nview_count(out, sizeof(out), 7);
    CHECK(strcmp(out, "7") == 0);
    nview_count(out, sizeof(out), 100);
    CHECK(strcmp(out, "99+") == 0);
}

int main(void) {
    test_queue();
    test_texts();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("notify_view_test: ok\n");
    return 0;
}
