// Host test: lichen_task_start's bookkeeping (#181): priority classes keep today's numbers and cores, names are cut to
// what FreeRTOS keeps (#123: names over 16 characters), slots are owned per app and counted for the stop check, and
// a stack goes to the memory asked for only while that region keeps its reserve (#269).
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

// The stack memory table (#269): names "fw tasks" prints and the reserves each region keeps
static void test_stack_classes(void) {
    CHECK(strcmp(task_stack_name(TASK_STACK_INTERNAL), "internal") == 0);
    CHECK(task_stack_reserve(TASK_STACK_INTERNAL) == 0);
    CHECK(strcmp(task_stack_name(TASK_STACK_LP_RAM), "lpram") == 0 && task_stack_reserve(TASK_STACK_LP_RAM) == 4096);
    CHECK(strcmp(task_stack_name(TASK_STACK_PSRAM), "psram") == 0 && task_stack_reserve(TASK_STACK_PSRAM) == 16384);
    CHECK(strcmp(task_stack_name(TASK_STACK_COUNT), "?") == 0 && task_stack_reserve(TASK_STACK_COUNT) == 0);
}

static void test_stack_place(void) {
    // Internal stays internal whatever the size
    CHECK(task_stack_place(TASK_STACK_INTERNAL, 4096, 0) == TASK_STACK_INTERNAL);
    // LP RAM while the stack plus the 4 KB reserve fits in the largest free block, internal from one byte short
    CHECK(task_stack_place(TASK_STACK_LP_RAM, 4096, 8192) == TASK_STACK_LP_RAM);
    CHECK(task_stack_place(TASK_STACK_LP_RAM, 4096, 8191) == TASK_STACK_INTERNAL);
    CHECK(task_stack_place(TASK_STACK_LP_RAM, 4096, 0) == TASK_STACK_INTERNAL);
    CHECK(task_stack_place(TASK_STACK_PSRAM, 8192, 24576) == TASK_STACK_PSRAM);
    CHECK(task_stack_place(TASK_STACK_PSRAM, 8192, 24575) == TASK_STACK_INTERNAL);
    // No wrap-round for a huge stack, and an unknown memory is internal
    CHECK(task_stack_place(TASK_STACK_LP_RAM, UINT32_MAX, 32768) == TASK_STACK_INTERNAL);
    CHECK(task_stack_place(TASK_STACK_COUNT, 1024, 1u << 20) == TASK_STACK_INTERNAL);
    CHECK(task_stack_place((task_stack_t)-1, 1024, 1u << 20) == TASK_STACK_INTERNAL);
}

// Stand-in contexts (lichen_ctx_t: slot + generation); the book only compares them
static task_owner_t const MESH = {0x12345601u, "meshcore"};
static task_owner_t const OS   = {0x00777700u, NULL};
static task_owner_t const NOTES = {0x00444403u, "notes"};

static void test_book(void) {
    static task_book_t b;
    memset(&b, 0, sizeof(b));
    int a = task_book_add(&b, MESH, "mesh", 12288, TASK_CLASS_APP_LOOP, TASK_STACK_INTERNAL, fn, &b);
    int j = task_book_add(&b, MESH, "meshrun", 8192, TASK_CLASS_JOB, TASK_STACK_INTERNAL, fn, NULL);
    int o = task_book_add(&b, OS, "battalert", 6144, TASK_CLASS_SERVICE, TASK_STACK_INTERNAL, fn, NULL);
    CHECK(a >= 0 && j >= 0 && o >= 0 && a != j && j != o);
    CHECK(strcmp(b.slot[o].owner, TASK_BOOK_OS) == 0 && b.slot[o].ctx == OS.ctx);
    CHECK(strcmp(b.slot[a].owner, "meshcore") == 0 && b.slot[a].ctx == MESH.ctx);
    CHECK(b.slot[a].fn == fn && b.slot[a].arg == &b && b.slot[a].stack == 12288 && b.slot[a].cls == TASK_CLASS_APP_LOOP);
    CHECK(task_book_add(&b, MESH, "x", 1024, TASK_CLASS_COUNT, TASK_STACK_INTERNAL, fn, NULL) == -1);
    CHECK(task_book_add(&b, MESH, "x", 1024, TASK_CLASS_JOB, TASK_STACK_COUNT, fn, NULL) == -1);
    CHECK(b.slot[a].mem == TASK_STACK_INTERNAL);
    int lp = task_book_add(&b, OS, "thermal", 4096, TASK_CLASS_SERVICE, TASK_STACK_LP_RAM, fn, NULL);
    CHECK(lp >= 0 && b.slot[lp].mem == TASK_STACK_LP_RAM && task_book_count(&b, OS.ctx, NULL) == 2);
    task_book_remove(&b, lp);
    CHECK(b.slot[lp].mem == TASK_STACK_INTERNAL && !b.slot[lp].used);
    CHECK(task_book_count(&b, MESH.ctx, NULL) == 2);
    CHECK(task_book_count(&b, OS.ctx, NULL) == 1);
    CHECK(task_book_count(&b, 0x00555502u, NULL) == 0);
    // The stopping job does not count itself
    int handle_j       = 1;
    b.slot[j].handle   = &handle_j;
    CHECK(task_book_count(&b, MESH.ctx, &handle_j) == 1);
    task_book_remove(&b, a);
    CHECK(task_book_count(&b, MESH.ctx, &handle_j) == 0);
    task_book_remove(&b, a);  // twice: nothing
    task_book_remove(&b, -1); // out of range: nothing
    task_book_remove(&b, TASK_BOOK_SLOTS);
    CHECK(task_book_count(&b, MESH.ctx, NULL) == 1);
    // An owner past 13 characters is cut like an id would be
    task_owner_t const long_id = {0x9909u, "an_overlong_owner_id"};
    int l = task_book_add(&b, long_id, "t", 1024, TASK_CLASS_JOB, TASK_STACK_INTERNAL, fn, NULL);
    CHECK(l >= 0 && strlen(b.slot[l].owner) == TASK_BOOK_OWNER_CAP - 1);
    // Full: -1, and a freed slot is used again
    memset(&b, 0, sizeof(b));
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        CHECK(task_book_add(&b, NOTES, "job", 1024, TASK_CLASS_JOB, TASK_STACK_INTERNAL, fn, NULL) == i);
    }
    CHECK(task_book_add(&b, NOTES, "job", 1024, TASK_CLASS_JOB, TASK_STACK_INTERNAL, fn, NULL) == -1);
    task_book_remove(&b, 7);
    CHECK(task_book_add(&b, NOTES, "job", 1024, TASK_CLASS_JOB, TASK_STACK_INTERNAL, fn, NULL) == 7);
}

// #230 E1b: tasks are counted by context, not label. Another context with the same label (a stale generation, or an
// app's id the OS also printed) is someone else; ctx 0 is never booked.
static void test_book_by_ctx(void) {
    static task_book_t b;
    memset(&b, 0, sizeof(b));
    task_owner_t const stale = {MESH.ctx ^ 0x100u, "meshcore"};
    int m = task_book_add(&b, MESH, "mesh", 4096, TASK_CLASS_APP_LOOP, TASK_STACK_INTERNAL, fn, NULL);
    int s = task_book_add(&b, stale, "mesh", 4096, TASK_CLASS_JOB, TASK_STACK_INTERNAL, fn, NULL);
    CHECK(m >= 0 && s >= 0);
    CHECK(task_book_count(&b, MESH.ctx, NULL) == 1 && task_book_count(&b, stale.ctx, NULL) == 1);
    task_book_remove(&b, s);
    CHECK(task_book_count(&b, MESH.ctx, NULL) == 1 && task_book_count(&b, stale.ctx, NULL) == 0);
    CHECK(task_book_add(&b, (task_owner_t){0, "meshcore"}, "x", 1024, TASK_CLASS_JOB, TASK_STACK_INTERNAL, fn, NULL) ==
          -1);
    CHECK(task_book_count(&b, 0, NULL) == 0);
}

int main(void) {
    test_book_by_ctx();
    test_classes();
    test_names();
    test_stack_classes();
    test_stack_place();
    test_book();
    if (failures) {
        printf("task_book_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("task_book_test: ok\n");
    return 0;
}
