// Host test: the gate's context check before the memory and task books (#230 E1b, gate_book.c), the code
// lichen_mem.c and lichen_task.c run: a refused context books, releases and starts nothing; the OS row is the OS's
// context, not a missing label; an app without an id is labelled by its name and keeps its quota; the trampoline runs
// a task as the context it was booked under.
#include "gate_book.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

// A fake context table: the OS, an app with an id, one without
#define OS_CTX    0x00a0a000u
#define MESH_CTX  0x00b0b001u
#define NOID_CTX  0x00c0c002u
#define STALE_CTX (MESH_CTX ^ 0x100u)

static unsigned refused, locks, unlocks, enters;
static uint32_t current; // the calling task's current context

static bool fake_resolve(uint32_t ctx, char const **id, char const **name, char const *what) {
    (void)what;
    if (ctx == OS_CTX) {
        *id = *name = NULL;
    } else if (ctx == MESH_CTX) {
        *id   = "meshcore";
        *name = "MeshCore";
    } else if (ctx == NOID_CTX) {
        *id   = NULL;
        *name = "Clock";
    } else {
        refused++; // 0, stale, made up
        return false;
    }
    return true;
}
static uint32_t fake_os(void) {
    return OS_CTX;
}
static uint32_t fake_enter(uint32_t ctx) {
    uint32_t prev = current;
    current       = ctx;
    enters++;
    return prev;
}
static void take(void) {
    locks++;
}
static void give(void) {
    unlocks++;
}
static gate_ctx_t const  g = {.resolve = fake_resolve, .os = fake_os, .enter = fake_enter};
static gate_lock_t const l = {.take = take, .give = give};

static void reset(void) {
    refused = locks = unlocks = enters = 0;
    current                            = 0;
}

static void test_owner(void) {
    reset();
    gate_owner_t who;
    CHECK(gate_owner(&g, OS_CTX, "t", &who) && who.os && who.label == NULL && who.ctx == OS_CTX);
    CHECK(gate_owner(&g, MESH_CTX, "t", &who) && !who.os && strcmp(who.label, "meshcore") == 0);
    CHECK(gate_owner(&g, NOID_CTX, "t", &who) && !who.os && strcmp(who.label, "Clock") == 0); // the name, not the OS
    CHECK(!gate_owner(&g, STALE_CTX, "t", &who) && !gate_owner(&g, 0, "t", &who) && refused == 2);
}

static void test_mem(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    reset();
    gate_owner_t who;
    // Refused: nothing booked, no lock taken (the caller then allocates nothing)
    CHECK(gate_mem_charge(&g, &l, &b, STALE_CTX, false, 100, &who) == MEM_GATE_REFUSED);
    CHECK(gate_mem_charge(&g, &l, &b, 0, false, 100, &who) == MEM_GATE_REFUSED);
    static mem_book_t const empty;
    CHECK(memcmp(&b, &empty, sizeof(b)) == 0 && locks == 0 && refused == 2);
    // Valid: charged under the ctx, with the lock
    CHECK(gate_mem_charge(&g, &l, &b, MESH_CTX, false, 4096, &who) == MEM_CHARGE_OK && who.ctx == MESH_CTX);
    CHECK(locks == 1 && unlocks == 1 && mem_book_find(&b, MESH_CTX)->bulk == 4096);
    // A refused free releases nothing: MeshCore's count stays (and the caller does not free the block)
    char const *label = "x";
    CHECK(gate_mem_release(&g, &l, &b, STALE_CTX, false, 4096, &label) == MEM_GATE_RELEASE_REFUSED);
    CHECK(mem_book_find(&b, MESH_CTX)->bulk == 4096 && mem_book_find(&b, MESH_CTX)->blocks == 1);
    CHECK(mem_book_find(&b, STALE_CTX) == NULL && locks == 1);
    CHECK(gate_mem_release(&g, &l, &b, MESH_CTX, false, 4096, &label) == MEM_RELEASE_OK);
    CHECK(strcmp(label, "meshcore") == 0 && mem_book_find(&b, MESH_CTX)->bulk == 0);
    // The OS has no quota; an app without an id has one, under its own row and name
    CHECK(gate_mem_charge(&g, &l, &b, OS_CTX, true, 2 * MEM_BOOK_INTERNAL_QUOTA, &who) == MEM_CHARGE_OK);
    CHECK(strcmp(mem_book_find(&b, OS_CTX)->owner, MEM_BOOK_OS) == 0);
    CHECK(gate_mem_charge(&g, &l, &b, NOID_CTX, true, MEM_BOOK_INTERNAL_QUOTA + 1, &who) == MEM_CHARGE_QUOTA);
    CHECK(mem_book_find(&b, NOID_CTX) && strcmp(mem_book_find(&b, NOID_CTX)->owner, "Clock") == 0);
    CHECK(!mem_book_find(&b, NOID_CTX)->os && mem_book_find(&b, NOID_CTX)->refused == 1);
}

static uint32_t ran_as;
static int      ran;
static void task_fn(void *arg) {
    ran_as = current;
    ran    = *(int *)arg;
}

static void test_task(void) {
    static task_book_t b;
    memset(&b, 0, sizeof(b));
    reset();
    int          token = 7;
    task_spec_t  spec  = {.name = "job", .stack = 4096, .cls = TASK_CLASS_JOB, .mem = TASK_STACK_INTERNAL,
                          .fn = task_fn, .arg = &token};
    gate_owner_t who;
    // Refused: the book is untouched, no lock taken
    CHECK(gate_task_add(&g, &l, &b, STALE_CTX, &spec, &who) == TASK_GATE_REFUSED);
    CHECK(gate_task_add(&g, &l, &b, 0, &spec, &who) == TASK_GATE_REFUSED);
    static task_book_t const empty;
    CHECK(memcmp(&b, &empty, sizeof(b)) == 0 && locks == 0);
    // Booked under the start ctx, with its label
    int s = gate_task_add(&g, &l, &b, MESH_CTX, &spec, &who);
    CHECK(s >= 0 && locks == 1 && unlocks == 1);
    CHECK(b.slot[s].ctx == MESH_CTX && strcmp(b.slot[s].owner, "meshcore") == 0);
    CHECK(task_book_count(&b, MESH_CTX, NULL) == 1);
    int o = gate_task_add(&g, &l, &b, OS_CTX, &spec, &who);
    CHECK(o >= 0 && strcmp(b.slot[o].owner, TASK_BOOK_OS) == 0 && who.os);
    // An invalid spec: -1 (not a refusal)
    spec.cls = TASK_CLASS_COUNT;
    CHECK(gate_task_add(&g, &l, &b, MESH_CTX, &spec, &who) == -1);
    // The trampoline: the new task (nothing entered) runs fn as the ctx it was booked under
    current = 0;
    gate_task_run(&g, b.slot[s].ctx, b.slot[s].fn, b.slot[s].arg);
    CHECK(enters == 1 && ran_as == MESH_CTX && ran == 7 && current == MESH_CTX);
    // ... and a task it starts with its own (lichen_ctx_self) is booked as MeshCore's too
    spec.cls  = TASK_CLASS_JOB;
    int n     = gate_task_add(&g, &l, &b, current, &spec, &who);
    CHECK(n >= 0 && task_book_count(&b, MESH_CTX, NULL) == 2);
    if (n < 0) {
        return;
    }
    CHECK(b.slot[n].ctx == MESH_CTX);
    current = 0;
    gate_task_run(&g, b.slot[n].ctx, task_fn, &token);
    CHECK(ran_as == MESH_CTX);
}

int main(void) {
    test_owner();
    test_mem();
    test_task();
    if (failures) {
        printf("gate_book_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("gate_book_test: all passed\n");
    return 0;
}
