// The UI service's call queue and lock-owner bookkeeping (#225), under the discipline lichen_ui_call.c keeps: every queue
// operation holds the (recursive) LVGL lock, producers on any thread, the drain on the "LVGL thread", a drain scheduled
// as an LVGL async call would be. Threaded (pthreads), so a lost wakeup, a double run or a reordering shows as a count.
#define _XOPEN_SOURCE 700 // PTHREAD_MUTEX_RECURSIVE
#include "ui_call_queue.h"
#include "ui_lock_hits.h"
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

// Cancel (#286, menu.c's row delete): every queued fn(arg) of that pair is dropped, other calls keep their order, and a
// call of the running batch that cancels a later one of the same batch leaves the batch's count right (the drain runs
// neither the cancelled call nor one queued after it started)
static ui_call_queue_t s_cancelq;
static void            cancel_three(void *arg) {
    note(arg);
    CHECK(ui_call_queue_cancel(&s_cancelq, note, (void *)3) == 1);
}

static void test_cancel(void) {
    ui_call_queue_t q = {0};
    ran_n             = 0;
    ui_call_queue_push(&q, note, (void *)1);
    ui_call_queue_push(&q, note, (void *)2);
    ui_call_queue_push(&q, note, (void *)1);
    ui_call_queue_push(&q, note, (void *)3);
    CHECK(ui_call_queue_cancel(&q, note, (void *)1) == 2);
    CHECK(ui_call_queue_cancel(&q, note, (void *)9) == 0);
    CHECK(ui_call_queue_cancel(&q, requeue, (void *)2) == 0); // the pair must match, not the arg alone
    CHECK(ui_call_queue_cancel(&q, NULL, (void *)2) == 0);
    ui_call_queue_drain(&q);
    CHECK(ran_n == 2 && ran[0] == 2 && ran[1] == 3);
    CHECK(!ui_call_queue_pop(&q, &(ui_call_t){0}));

    memset(&s_cancelq, 0, sizeof(s_cancelq));
    ran_n = 0;
    ui_call_queue_push(&s_cancelq, cancel_three, (void *)1);
    ui_call_queue_push(&s_cancelq, note, (void *)3);
    ui_call_queue_push(&s_cancelq, note, (void *)4);
    ui_call_queue_drain(&s_cancelq);
    CHECK(ran_n == 2 && ran[0] == 1 && ran[1] == 4);
    CHECK(s_cancelq.count == 0);
}

// ---- threads: producers, one LVGL thread, the recursive lock, the production hand-over ----

#define PRODUCERS 6
#define PER_PRODUCER 4000
#define RESPAWN_EVERY 97      // every so many calls queues another from inside the drain
#define SCHEDULE_FAIL_EVERY 7 // every so many drain schedules fail (lv_async_call out of memory)
#define FALLBACK_BIT ((uintptr_t)1 << 30)

static pthread_mutex_t lvgl_lock;  // the port's recursive mutex
static pthread_cond_t  timer_cond; // an LVGL async timer is due
static ui_call_queue_t s_q;        // lvgl_lock held
static atomic_bool     s_stop;

// The fake LVGL timer list (lvgl_lock held): drains and fallback calls, each an async timer that runs once
typedef struct timer {
    ui_call_t     call;
    struct timer *next;
} timer_t_;
static timer_t_ *s_timer_head, **s_timer_tail = &s_timer_head;
static int       s_schedules, s_schedule_fails, s_fallbacks; // lvgl_lock held

static int s_last_seq[PRODUCERS];                     // LVGL thread
static bool s_rejected[PRODUCERS][PER_PRODUCER + 1];  // lvgl_lock held: hand-over said false, so it must never run
static int s_runs, s_rejects, s_respawns, s_respawn_runs, s_order_errors, s_ran_rejected; // lvgl_lock held
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

static bool add_timer(ui_call_fn_t fn, void *arg) {
    timer_t_ *t = malloc(sizeof(*t));
    if (!t) {
        return false;
    }
    *t            = (timer_t_){{fn, arg}, NULL};
    *s_timer_tail = t;
    s_timer_tail  = &t->next;
    pthread_cond_signal(&timer_cond);
    return true;
}

static bool fake_schedule(ui_call_fn_t fn, void *arg) {
    if (++s_schedules % SCHEDULE_FAIL_EVERY == 0) {
        s_schedule_fails++;
        return false;
    }
    return add_timer(fn, arg);
}

static bool fake_fallback(ui_call_fn_t fn, void *arg) {
    s_fallbacks++;
    return add_timer(fn, (void *)((uintptr_t)arg | FALLBACK_BIT)); // marked: a fallback may overtake the queue
}

// lichen_sys_ui_call: the lock, the check an allocation would make, the production hand-over
static bool sys_ui_call(ui_call_fn_t fn, void *arg, uintptr_t self) {
    lock_take(self);
    touch_lvgl(self);
    bool ok = ui_call_queue_hand_over(&s_q, fn, arg, fake_schedule, fake_fallback);
    lock_give(self);
    return ok;
}

static void respawned(void *arg) {
    (void)arg;
    s_respawn_runs++;
}

static void counted(void *arg) {
    uintptr_t v        = (uintptr_t)arg;
    bool      fallback = v & FALLBACK_BIT;
    int       who = (int)((v & ~FALLBACK_BIT) >> 20), seq = (int)(v & 0xFFFFF);
    if (s_rejected[who][seq]) {
        s_ran_rejected++; // the caller was told it would not run and kept arg
    }
    if (!fallback) { // per producer FIFO through the queue (rejected calls leave gaps, never reorder)
        if (seq <= s_last_seq[who]) {
            s_order_errors++;
        }
        s_last_seq[who] = seq;
    }
    s_runs++;
    if (seq % RESPAWN_EVERY == 0 && sys_ui_call(respawned, NULL, s_lvgl_id)) { // re-entrant: the lock is held
        s_respawns++;
    }
}

static void *producer(void *arg) {
    int       who  = (int)(intptr_t)arg;
    uintptr_t self = (uintptr_t)(who + 1);
    for (int seq = 1; seq <= PER_PRODUCER; seq++) {
        if (!sys_ui_call(counted, (void *)(((uintptr_t)who << 20) | (uintptr_t)seq), self)) {
            pthread_mutex_lock(&lvgl_lock);
            s_rejected[who][seq] = true;
            s_rejects++;
            pthread_mutex_unlock(&lvgl_lock);
        }
        touch_lvgl(self); // outside the lock: what the check must catch
    }
    return NULL;
}

// The port task: holds the lock around lv_timer_handler without the UI service, runs the due timers one by one
static void *lvgl_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lvgl_lock);
    for (;;) {
        while (!s_timer_head && !atomic_load(&s_stop)) {
            pthread_cond_wait(&timer_cond, &lvgl_lock);
        }
        timer_t_ *t = s_timer_head;
        if (!t) {
            break;
        }
        s_timer_head = t->next;
        if (!s_timer_head) {
            s_timer_tail = &s_timer_head;
        }
        touch_lvgl(s_lvgl_id);
        t->call.fn(t->call.arg);
        free(t);
    }
    pthread_mutex_unlock(&lvgl_lock);
    return NULL;
}

static void test_threads(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lvgl_lock, &attr);
    pthread_cond_init(&timer_cond, NULL);
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
    // Every accepted call has a drain on its way: the LVGL thread empties the queue with no extra kick
    pthread_mutex_lock(&lvgl_lock);
    atomic_store(&s_stop, true);
    pthread_cond_signal(&timer_cond);
    pthread_mutex_unlock(&lvgl_lock);
    pthread_join(lv, NULL);
    ui_call_t c;
    CHECK(!ui_call_queue_pop(&s_q, &c)); // nothing stranded (a lost wakeup, or a failed schedule left queued)
    CHECK(!s_q.scheduled);
    CHECK(s_schedule_fails > 0);
    CHECK(s_runs == PRODUCERS * PER_PRODUCER - s_rejects);
    CHECK(s_ran_rejected == 0);
    CHECK(s_respawn_runs == s_respawns);
    CHECK(s_order_errors == 0);
    // Each producer touched LVGL outside the lock once per call; inside it never
    CHECK(atomic_load(&s_violations) == PRODUCERS * PER_PRODUCER);
    printf("threads: %d runs, %d rejected (%d schedules failed), %d respawned, %d fallbacks\n", s_runs, s_rejects,
           s_schedule_fails, s_respawns, s_fallbacks);
}

// ---- count mode's hit table: many tasks, a few sites, more sites than slots ----

#define HIT_THREADS 8
#define HIT_ROUNDS 20000
#define HIT_SITE_KINDS 12 // more than UI_LOCK_HIT_SITES: the rest only count in total

static ui_lock_hits_t s_hits;
static atomic_int     s_first_hits;

static void *hitter(void *arg) {
    int who = (int)(intptr_t)arg;
    for (int i = 0; i < HIT_ROUNDS; i++) {
        uintptr_t site = (uintptr_t)(0x1000 + 0x10 * ((i + who) % HIT_SITE_KINDS));
        if (ui_lock_hits_record(&s_hits, site, "task", "lv_malloc")) {
            atomic_fetch_add(&s_first_hits, 1);
        }
    }
    return NULL;
}

static void test_hits(void) {
    pthread_t t[HIT_THREADS];
    for (int i = 0; i < HIT_THREADS; i++) {
        pthread_create(&t[i], NULL, hitter, (void *)(intptr_t)i);
    }
    for (int i = 0; i < HIT_THREADS; i++) {
        pthread_join(t[i], NULL);
    }
    CHECK(atomic_load(&s_hits.total) == HIT_THREADS * HIT_ROUNDS);
    CHECK(atomic_load(&s_first_hits) == UI_LOCK_HIT_SITES); // one log line per site that got a slot, never two
    uint32_t in_slots = 0;
    for (int i = 0; i < UI_LOCK_HIT_SITES; i++) {
        ui_lock_hit_site_t *s = &s_hits.sites[i];
        CHECK(atomic_load(&s->site) != 0 && atomic_load(&s->ready) && strcmp(s->task, "task") == 0);
        for (int j = 0; j < i; j++) {
            CHECK(atomic_load(&s_hits.sites[j].site) != atomic_load(&s->site)); // no site twice
        }
        in_slots += atomic_load(&s->count);
    }
    CHECK(in_slots < atomic_load(&s_hits.total)); // the sites past the table only counted in total
    ui_lock_hits_reset(&s_hits);
    CHECK(atomic_load(&s_hits.total) == 0 && atomic_load(&s_hits.sites[0].site) == 0);
    CHECK(ui_lock_hits_record(&s_hits, 0x42, "a-task-name-longer-than-sixteen", "lv_free")); // a fresh first hit
    CHECK(strlen(s_hits.sites[0].task) == UI_LOCK_HIT_TASK_LEN - 1);
}

// ---- lock owner: recursion, foreign unlock, unarmed ----

// #248: no display, no port: a lock request is refused (and reported once) until the boot step marks the port up
static void test_port_gate(void) {
    ui_lock_owner_t o = {0};
    bool            first;
    CHECK(!ui_lock_owner_port_is_up(&o));
    CHECK(!ui_lock_owner_may_lock(&o, &first));
    CHECK(first);
    CHECK(!ui_lock_owner_may_lock(&o, &first));
    CHECK(!first); // logged once
    ui_lock_owner_set_port_up(&o);
    CHECK(ui_lock_owner_port_is_up(&o));
    CHECK(ui_lock_owner_may_lock(&o, &first));
    CHECK(!first);
}

static void test_owner(void) {
    ui_lock_owner_t o = {0};
    CHECK(!ui_lock_owner_armed(&o));
    CHECK(ui_lock_owner_check(&o, 5) == UI_LOCK_UNARMED); // boot, before the display and keyboard are set up
    ui_lock_owner_arm(&o, 1);
    CHECK(ui_lock_owner_armed(&o));
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
    test_cancel();
    test_owner();
    test_port_gate();
    test_threads();
    test_hits();
    if (failures) {
        printf("ui_call_queue_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("ui_call_queue_test: all passed\n");
    return 0;
}
