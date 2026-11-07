// The UI service's call queue and lock-owner bookkeeping (#225), under the discipline lichen_ui_call.c keeps: every queue
// operation holds the (recursive) LVGL lock, producers on any thread, the drain on the "LVGL thread", a drain scheduled
// as an LVGL async call would be. Threaded (pthreads), so a lost wakeup, a double run or a reordering shows as a count.
#define _XOPEN_SOURCE 700 // PTHREAD_MUTEX_RECURSIVE
#include "ui_call_queue.h"
#include "ui_lock_owner.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

// ---- single thread: the queue's own rules ----

static int ran[64], ran_n;
static void note(void *arg) {
    ran[ran_n++] = (int)(intptr_t)arg;
}

static void test_fifo_and_schedule(void) {
    ui_call_queue_t q = {0};
    ran_n             = 0;
    CHECK(ui_call_queue_push(&q, note, (void *)1) == UI_CALL_QUEUED_SCHEDULE); // the first schedules the drain
    CHECK(ui_call_queue_push(&q, note, (void *)2) == UI_CALL_QUEUED);          // the rest ride along
    CHECK(ui_call_queue_push(&q, note, (void *)3) == UI_CALL_QUEUED);
    size_t    n = ui_call_queue_begin_drain(&q);
    ui_call_t c;
    CHECK(n == 3);
    while (n-- > 0 && ui_call_queue_pop(&q, &c)) {
        c.fn(c.arg);
    }
    CHECK(ran_n == 3 && ran[0] == 1 && ran[1] == 2 && ran[2] == 3); // in the order they came (lv_async_call: newest first)
    CHECK(!ui_call_queue_pop(&q, &c));
    CHECK(ui_call_queue_push(&q, note, (void *)4) == UI_CALL_QUEUED_SCHEDULE); // drained: the next one schedules again
}

static void test_full_and_unpush(void) {
    ui_call_queue_t q = {0};
    for (int i = 0; i < UI_CALL_QUEUE_CAP; i++) {
        CHECK(ui_call_queue_push(&q, note, (void *)(intptr_t)i) != UI_CALL_FULL);
    }
    CHECK(ui_call_queue_push(&q, note, NULL) == UI_CALL_FULL); // the caller falls back to an async call of its own
    ui_call_queue_t r = {0};
    CHECK(ui_call_queue_push(&r, note, (void *)7) == UI_CALL_QUEUED_SCHEDULE);
    ui_call_queue_unpush(&r); // scheduling the drain failed: as if never pushed
    ui_call_t c;
    CHECK(!ui_call_queue_pop(&r, &c));
    CHECK(ui_call_queue_push(&r, note, (void *)8) == UI_CALL_QUEUED_SCHEDULE); // and the next push schedules again
}

// A call that queues another while the drain runs: the new one goes to the next drain, never inline, and it is
// scheduled even though calls of the current batch are still waiting (the lost wakeup a "was empty" test would have)
static ui_call_queue_t s_reentrant;
static int             s_child_scheduled;
static void            requeue(void *arg) {
    note(arg);
    if (ui_call_queue_push(&s_reentrant, note, (void *)99) == UI_CALL_QUEUED_SCHEDULE) {
        s_child_scheduled++;
    }
}

static void test_reentrant_push(void) {
    memset(&s_reentrant, 0, sizeof(s_reentrant));
    ran_n             = 0;
    s_child_scheduled = 0;
    ui_call_queue_push(&s_reentrant, requeue, (void *)1);
    ui_call_queue_push(&s_reentrant, note, (void *)2); // still queued when requeue runs
    size_t    n = ui_call_queue_begin_drain(&s_reentrant);
    ui_call_t c;
    while (n-- > 0 && ui_call_queue_pop(&s_reentrant, &c)) {
        c.fn(c.arg);
    }
    CHECK(ran_n == 2 && ran[0] == 1 && ran[1] == 2); // 99 did not run inside this drain
    CHECK(s_child_scheduled == 1);
    n = ui_call_queue_begin_drain(&s_reentrant);
    CHECK(n == 1 && ui_call_queue_pop(&s_reentrant, &c) && c.arg == (void *)99);
}

// ---- threads: producers, one LVGL thread, the recursive lock ----

#define PRODUCERS 6
#define PER_PRODUCER 4000
#define RESPAWN_EVERY 97 // every so many calls queues another from inside the drain
#define FALLBACK_BIT ((uintptr_t)1 << 30)

static pthread_mutex_t lvgl_lock;  // the port's recursive mutex
static pthread_cond_t  drain_cond; // an LVGL async timer that is due
static int             drains_due; // lvgl_lock held
static ui_call_queue_t s_q;        // lvgl_lock held
static atomic_bool     s_stop;
static int             last_seq[PRODUCERS]; // LVGL thread only
typedef struct fallback {
    ui_call_t        call;
    struct fallback *next;
} fallback_t;
static fallback_t     *s_fallback_head, **s_fallback_tail = &s_fallback_head; // lvgl_lock held: own async timers
static int             s_runs, s_respawns, s_respawn_runs, s_fallbacks, s_order_errors; // LVGL thread / lvgl_lock
static ui_lock_owner_t s_owner;
static uintptr_t       s_lvgl_id;
static atomic_int      s_violations;

static void lock_take(uintptr_t self) {
    pthread_mutex_lock(&lvgl_lock);
    ui_lock_owner_taken(&s_owner, self);
}

static void lock_give(uintptr_t self) {
    CHECK(ui_lock_owner_given(&s_owner, self));
    pthread_mutex_unlock(&lvgl_lock);
}

static void touch_lvgl(uintptr_t self) { // what the debug check sees on a wrapped LVGL call
    if (ui_lock_owner_check(&s_owner, self) == UI_LOCK_VIOLATION) {
        atomic_fetch_add(&s_violations, 1);
    }
}

// lichen_sys_ui_call's hand-over, lock held: the drain is "scheduled" as an async timer the LVGL thread will run
static void hand_over(ui_call_fn_t fn, void *arg) {
    switch (ui_call_queue_push(&s_q, fn, arg)) {
    case UI_CALL_QUEUED_SCHEDULE:
        drains_due++;
        pthread_cond_signal(&drain_cond);
        break;
    case UI_CALL_FULL: { // the fallback: an async timer of its own, which may overtake the queue
        fallback_t *f = malloc(sizeof(*f));
        CHECK(f != NULL);
        if (!f) {
            break;
        }
        *f = (fallback_t){{fn, (void *)((uintptr_t)arg | FALLBACK_BIT)}, NULL};
        *s_fallback_tail = f;
        s_fallback_tail  = &f->next;
        s_fallbacks++;
        drains_due++;
        pthread_cond_signal(&drain_cond);
        break;
    }
    case UI_CALL_QUEUED: break;
    }
}

static void sys_ui_call(ui_call_fn_t fn, void *arg, uintptr_t self) {
    lock_take(self);
    touch_lvgl(self); // lv_async_call allocates: must pass the check
    hand_over(fn, arg);
    lock_give(self);
}

static void respawned(void *arg) {
    (void)arg;
    s_respawn_runs++;
}

static void counted(void *arg) {
    uintptr_t v        = (uintptr_t)arg;
    bool      fallback = v & FALLBACK_BIT;
    int       who = (int)((v & ~FALLBACK_BIT) >> 20), seq = (int)(v & 0xFFFFF);
    if (!fallback) { // per producer FIFO through the queue; a fallback timer is outside it (as lv_async_call was)
        if (seq <= last_seq[who]) {
            s_order_errors++;
        }
        last_seq[who] = seq;
    }
    s_runs++;
    if (seq % RESPAWN_EVERY == 0) {
        s_respawns++;
        sys_ui_call(respawned, NULL, s_lvgl_id); // re-entrant: the LVGL thread already holds the recursive lock
    }
}

static void *producer(void *arg) {
    int       who  = (int)(intptr_t)arg;
    uintptr_t self = (uintptr_t)(who + 1);
    for (int seq = 1; seq <= PER_PRODUCER; seq++) {
        sys_ui_call(counted, (void *)(((uintptr_t)who << 20) | (uintptr_t)seq), self);
        touch_lvgl(self); // outside the lock: what the check must catch
    }
    return NULL;
}

static void *lvgl_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lvgl_lock); // the port holds the lock around lv_timer_handler without the UI service
    for (;;) {
        while (drains_due == 0 && !atomic_load(&s_stop)) {
            pthread_cond_wait(&drain_cond, &lvgl_lock);
        }
        if (drains_due == 0) {
            break;
        }
        drains_due--;
        touch_lvgl(s_lvgl_id);
        if (s_fallback_head) { // the timers are due in turn; which one first does not matter here
            fallback_t *f   = s_fallback_head;
            s_fallback_head = f->next;
            if (!s_fallback_head) {
                s_fallback_tail = &s_fallback_head;
            }
            f->call.fn(f->call.arg);
            free(f);
            continue;
        }
        size_t    n = ui_call_queue_begin_drain(&s_q);
        ui_call_t c;
        while (n-- > 0 && ui_call_queue_pop(&s_q, &c)) {
            c.fn(c.arg);
        }
    }
    pthread_mutex_unlock(&lvgl_lock);
    return NULL;
}

static void test_threads(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lvgl_lock, &attr);
    pthread_cond_init(&drain_cond, NULL);
    s_lvgl_id = 1000;
    ui_lock_owner_arm(&s_owner, s_lvgl_id);
    pthread_t lv, p[PRODUCERS];
    pthread_create(&lv, NULL, lvgl_thread, NULL);
    for (int i = 0; i < PRODUCERS; i++) {
        pthread_create(&p[i], NULL, producer, (void *)(intptr_t)i);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        pthread_join(p[i], NULL);
    }
    // Every call queued has a drain on its way: the LVGL thread empties the queue with no extra kick
    pthread_mutex_lock(&lvgl_lock);
    atomic_store(&s_stop, true);
    pthread_cond_signal(&drain_cond);
    pthread_mutex_unlock(&lvgl_lock);
    pthread_join(lv, NULL);
    ui_call_t c;
    CHECK(!ui_call_queue_pop(&s_q, &c)); // nothing stranded (a lost wakeup)
    CHECK(!s_q.scheduled);
    CHECK(s_runs == PRODUCERS * PER_PRODUCER);
    CHECK(s_respawn_runs == s_respawns);
    CHECK(s_order_errors == 0);
    // Each producer touched LVGL outside the lock once per call; inside it never
    CHECK(atomic_load(&s_violations) == PRODUCERS * PER_PRODUCER);
    printf("threads: %d runs, %d respawned, %d fallbacks\n", s_runs, s_respawns, s_fallbacks);
}

// ---- lock owner: recursion, foreign unlock, unarmed ----

static void test_owner(void) {
    ui_lock_owner_t o = {0};
    CHECK(ui_lock_owner_check(&o, 5) == UI_LOCK_UNARMED); // boot, before the display and keyboard are set up
    ui_lock_owner_arm(&o, 1);
    CHECK(ui_lock_owner_check(&o, 1) == UI_LOCK_OK); // the LVGL task, lock or not
    CHECK(ui_lock_owner_check(&o, 5) == UI_LOCK_VIOLATION);
    ui_lock_owner_taken(&o, 5);
    ui_lock_owner_taken(&o, 5); // recursive
    CHECK(ui_lock_owner_check(&o, 5) == UI_LOCK_OK);
    CHECK(ui_lock_owner_check(&o, 6) == UI_LOCK_VIOLATION);
    CHECK(!ui_lock_owner_given(&o, 6)); // not its lock
    CHECK(ui_lock_owner_given(&o, 5));
    CHECK(ui_lock_owner_check(&o, 5) == UI_LOCK_OK); // still one level held
    CHECK(ui_lock_owner_given(&o, 5));
    CHECK(ui_lock_owner_check(&o, 5) == UI_LOCK_VIOLATION);
    CHECK(!ui_lock_owner_given(&o, 5)); // one unlock too many
}

int main(void) {
    test_fifo_and_schedule();
    test_full_and_unpush();
    test_reentrant_push();
    test_owner();
    test_threads();
    if (failures) {
        printf("ui_call_queue_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("ui_call_queue_test: all passed\n");
    return 0;
}
