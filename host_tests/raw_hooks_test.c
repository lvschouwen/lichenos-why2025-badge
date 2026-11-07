// The raw key hook table (#251): once raw_hooks_remove returns (with a wait), no call of the hook is running or will
// start, so its owner may end (factory_combo's task) without the keyboard task notifying a freed task. Threaded
// (pthreads): one "keyboard task" runs the hooks while other threads remove them.
#define _POSIX_C_SOURCE 200809L // nanosleep
#include "raw_hooks.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static key_raw_event_t const EV = {.code = 1, .pressed = true};

static void yield(void) {
    sched_yield();
}

// ---- single thread: the table's own rules ----

static int calls[RAW_HOOKS_MAX + 1];
static bool count(key_raw_event_t const *ev, void *arg) {
    (void)ev;
    calls[(int)(intptr_t)arg]++;
    return (intptr_t)arg == 2; // hook 2 swallows
}

static void test_add_run_remove(void) {
    raw_hooks_t h = {0};
    CHECK(!raw_hooks_run(&h, &EV)); // empty: nothing swallows
    for (int i = 0; i < RAW_HOOKS_MAX; i++) {
        CHECK(raw_hooks_add(&h, count, (void *)(intptr_t)i));
    }
    CHECK(!raw_hooks_add(&h, count, (void *)(intptr_t)RAW_HOOKS_MAX)); // full
    CHECK(raw_hooks_run(&h, &EV));                                        // hook 2 swallowed, the others still saw it
    for (int i = 0; i < RAW_HOOKS_MAX; i++) {
        CHECK(calls[i] == 1);
    }
    raw_hooks_remove(&h, count, (void *)2, yield);
    CHECK(!raw_hooks_run(&h, &EV)); // the swallower is gone
    CHECK(calls[2] == 1 && calls[0] == 2);
    raw_hooks_remove(&h, count, (void *)99, yield); // not there: nothing happens
    CHECK(raw_hooks_add(&h, count, (void *)(intptr_t)RAW_HOOKS_MAX)); // its slot is free again
}

static void test_remove_takes_every_copy(void) {
    raw_hooks_t h = {0};
    calls[0]      = 0;
    CHECK(raw_hooks_add(&h, count, (void *)0) && raw_hooks_add(&h, count, (void *)0));
    raw_hooks_run(&h, &EV);
    CHECK(calls[0] == 2);
    raw_hooks_remove(&h, count, (void *)0, yield);
    raw_hooks_run(&h, &EV);
    CHECK(calls[0] == 2);
}

// A hook removing itself, and another one, from the calling task: wait = NULL, no deadlock
static raw_hooks_t self_table;
static int         self_calls, other_calls;
static bool other(key_raw_event_t const *ev, void *arg) {
    (void)ev;
    (void)arg;
    other_calls++;
    return false;
}
static bool remove_self(key_raw_event_t const *ev, void *arg) {
    (void)ev;
    (void)arg;
    self_calls++;
    raw_hooks_remove(&self_table, remove_self, NULL, NULL);
    raw_hooks_remove(&self_table, other, NULL, NULL);
    return true;
}

static void test_remove_from_inside_a_hook(void) {
    CHECK(raw_hooks_add(&self_table, remove_self, NULL));
    CHECK(raw_hooks_add(&self_table, other, NULL));
    CHECK(raw_hooks_run(&self_table, &EV)); // its own call still counts
    CHECK(self_calls == 1 && other_calls == 0); // removed before its turn came
    CHECK(!raw_hooks_run(&self_table, &EV));
    CHECK(self_calls == 1 && other_calls == 0);
}

// ---- threads: remove waits for the call in flight ----

static raw_hooks_t blocking_table;
static atomic_bool in_hook, release_hook, remove_done;
static bool block(key_raw_event_t const *ev, void *arg) {
    (void)ev;
    (void)arg;
    atomic_store(&in_hook, true);
    while (!atomic_load(&release_hook)) {
        sched_yield();
    }
    return false;
}
static void *run_once(void *arg) {
    (void)arg;
    raw_hooks_run(&blocking_table, &EV);
    return NULL;
}
static void *remove_block(void *arg) {
    (void)arg;
    raw_hooks_remove(&blocking_table, block, NULL, yield);
    atomic_store(&remove_done, true);
    return NULL;
}

static void test_remove_waits_for_a_running_call(void) {
    CHECK(raw_hooks_add(&blocking_table, block, NULL));
    pthread_t caller, remover;
    pthread_create(&caller, NULL, run_once, NULL);
    while (!atomic_load(&in_hook)) {
        sched_yield();
    }
    pthread_create(&remover, NULL, remove_block, NULL);
    nanosleep(&(struct timespec){.tv_nsec = 50 * 1000 * 1000}, NULL);
    CHECK(!atomic_load(&remove_done)); // the hook is still running: remove must not have returned
    atomic_store(&release_hook, true);
    pthread_join(caller, NULL);
    pthread_join(remover, NULL);
    CHECK(atomic_load(&remove_done));
}

// A remover racing the caller: a hook's context is marked dead once remove returns; no call may see it dead
#define ROUNDS 3000
static raw_hooks_t race_table;
static atomic_bool dead[ROUNDS], stop;
static atomic_int  late_calls, race_calls;
static bool check_alive(key_raw_event_t const *ev, void *arg) {
    (void)ev;
    atomic_bool *d = arg;
    if (atomic_load(d)) {
        atomic_fetch_add(&late_calls, 1);
    }
    for (volatile int i = 0; i < 200; i++) { // widen the window a remover could slip through
    }
    if (atomic_load(d)) {
        atomic_fetch_add(&late_calls, 1);
    }
    atomic_fetch_add(&race_calls, 1);
    return false;
}
static void *keyboard_task(void *arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        raw_hooks_run(&race_table, &EV);
    }
    return NULL;
}
static void *owner(void *arg) {
    int base = (int)(intptr_t)arg;
    for (int i = base; i < ROUNDS; i += 2) {
        while (!raw_hooks_add(&race_table, check_alive, &dead[i])) {
            sched_yield();
        }
        for (int y = 0; y < (i % 7); y++) {
            sched_yield();
        }
        raw_hooks_remove(&race_table, check_alive, &dead[i], yield);
        atomic_store(&dead[i], true); // the owner frees what the hook touches
    }
    return NULL;
}

static void test_remover_races_the_caller(void) {
    pthread_t kb, a, b;
    pthread_create(&kb, NULL, keyboard_task, NULL);
    pthread_create(&a, NULL, owner, (void *)0);
    pthread_create(&b, NULL, owner, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    atomic_store(&stop, true);
    pthread_join(kb, NULL);
    CHECK(atomic_load(&late_calls) == 0);
    CHECK(atomic_load(&race_calls) > 0); // the hooks did run meanwhile
    printf("race: %d calls, %d after remove\n", atomic_load(&race_calls), atomic_load(&late_calls));
}

int main(void) {
    test_add_run_remove();
    test_remove_takes_every_copy();
    test_remove_from_inside_a_hook();
    test_remove_waits_for_a_running_call();
    test_remover_races_the_caller();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("raw_hooks_test: ok\n");
    return 0;
}
