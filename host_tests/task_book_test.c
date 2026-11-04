// Host test: lichen_task_start's bookkeeping (#181): priority classes keep today's numbers and cores, names are cut to
// what FreeRTOS keeps (#123: names over 16 characters), slots are owned per app and counted for the stop check.
#include "task_book.h"

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

static void fn(void *arg) {
    (void)arg;
}

static void test_classes(void) {
    CHECK(task_class_priority(TASK_CLASS_BACKGROUND) == 2 && task_class_core(TASK_CLASS_BACKGROUND) == 0);
    CHECK(task_class_priority(TASK_CLASS_SERVICE) == 3 && task_class_core(TASK_CLASS_SERVICE) == 0);
    CHECK(task_class_priority(TASK_CLASS_JOB) == 4 && task_class_core(TASK_CLASS_JOB) == 0);
    CHECK(task_class_priority(TASK_CLASS_URGENT) == 5 && task_class_core(TASK_CLASS_URGENT) == 0);
    CHECK(task_class_priority(TASK_CLASS_APP_LOOP) == 3 && task_class_core(TASK_CLASS_APP_LOOP) == 1);
    CHECK(task_class_priority(TASK_CLASS_COUNT) == -1 && task_class_core((task_class_t)-1) == -1);
}

static void test_names(void) {
    char out[TASK_BOOK_NAME_CAP];
    CHECK(!task_name_fit(out, "meshrun") && strcmp(out, "meshrun") == 0);
    CHECK(!task_name_fit(out, "fifteen_chars__") && strcmp(out, "fifteen_chars__") == 0);
    CHECK(task_name_fit(out, "sixteen_chars___") && strcmp(out, "sixteen_chars__") == 0);
    CHECK(task_name_fit(out, "a_much_longer_task_name") && strlen(out) == TASK_BOOK_NAME_CAP - 1);
    CHECK(!task_name_fit(out, NULL) && out[0] == 0);
}

static void test_book(void) {
    static task_book_t b;
    memset(&b, 0, sizeof(b));
    int a = task_book_add(&b, "meshcore", "mesh", 12288, TASK_CLASS_APP_LOOP, fn, &b);
    int j = task_book_add(&b, "meshcore", "meshrun", 8192, TASK_CLASS_JOB, fn, NULL);
    int o = task_book_add(&b, NULL, "battalert", 6144, TASK_CLASS_SERVICE, fn, NULL);
    CHECK(a >= 0 && j >= 0 && o >= 0 && a != j && j != o);
    CHECK(strcmp(b.slot[o].owner, TASK_BOOK_OS) == 0);
    CHECK(b.slot[a].fn == fn && b.slot[a].arg == &b && b.slot[a].stack == 12288 && b.slot[a].cls == TASK_CLASS_APP_LOOP);
    CHECK(task_book_add(&b, "meshcore", "x", 1024, TASK_CLASS_COUNT, fn, NULL) == -1);
    CHECK(task_book_count(&b, "meshcore", NULL) == 2);
    CHECK(task_book_count(&b, NULL, NULL) == 1);
    CHECK(task_book_count(&b, "weather", NULL) == 0);
    // The stopping job does not count itself
    int handle_j       = 1;
    b.slot[j].handle   = &handle_j;
    CHECK(task_book_count(&b, "meshcore", &handle_j) == 1);
    task_book_remove(&b, a);
    CHECK(task_book_count(&b, "meshcore", &handle_j) == 0);
    task_book_remove(&b, a);  // twice: nothing
    task_book_remove(&b, -1); // out of range: nothing
    task_book_remove(&b, TASK_BOOK_SLOTS);
    CHECK(task_book_count(&b, "meshcore", NULL) == 1);
    // An owner past 13 characters is cut like an id would be
    int l = task_book_add(&b, "an_overlong_owner_id", "t", 1024, TASK_CLASS_JOB, fn, NULL);
    CHECK(l >= 0 && strlen(b.slot[l].owner) == TASK_BOOK_OWNER_CAP - 1);
    // Full: -1, and a freed slot is used again
    memset(&b, 0, sizeof(b));
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        CHECK(task_book_add(&b, "notes", "job", 1024, TASK_CLASS_JOB, fn, NULL) == i);
    }
    CHECK(task_book_add(&b, "notes", "job", 1024, TASK_CLASS_JOB, fn, NULL) == -1);
    task_book_remove(&b, 7);
    CHECK(task_book_add(&b, "notes", "job", 1024, TASK_CLASS_JOB, fn, NULL) == 7);
}

int main(void) {
    test_classes();
    test_names();
    test_book();
    if (failures) {
        printf("task_book_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("task_book_test: ok\n");
    return 0;
}
