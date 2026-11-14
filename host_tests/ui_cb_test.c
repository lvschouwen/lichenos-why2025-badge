// Event and timer callbacks run as the context that registered them (#349, ui_cb.c) over the real context table
// (app_ctx.c): the registering context is kept, entered around the call and the caller's put back exactly, also when
// they nest or the callback deletes its own record; no allocation on the call path; every record is freed.
#include "app_ctx.h"
#include "ui_cb.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

struct _lv_event_t {
    int code;
};
struct _lv_timer_t {
    int ticks;
};
struct _lv_group_t {
    int moves;
};

static int const app_a = 1, app_b = 2;
static app_ctx_table_t s_t;
static uint32_t        s_slot; // the "task's" thread-local slot
static int             s_live, s_allocs, s_fail_next;

static uint32_t tls_get(void) { return s_slot; }
static void     tls_set(uint32_t ctx) { s_slot = ctx; }
static bool     tls_isr(void) { return false; }
static app_ctx_tls_t const s_tls = {.get = tls_get, .set = tls_set, .in_isr = tls_isr};

static void *count_alloc(size_t size) {
    if (s_fail_next) {
        s_fail_next = 0;
        return NULL;
    }
    s_allocs++;
    s_live++;
    return malloc(size);
}
static void count_free(void *p) {
    s_live--;
    free(p);
}
static uint32_t self(void) { return app_ctx_self(&s_t, &s_tls); }
static uint32_t enter(uint32_t ctx) { return app_ctx_enter(&s_t, &s_tls, ctx, NULL); }
static void     leave(uint32_t previous) { app_ctx_leave(&s_tls, previous); }

// The trampoline's pair as lichen_ui_cb.c has it: the mark before the enter, cleared after the leave
static ui_cb_mark_t s_mark;
static int          s_task;
static ui_cb_saved_t env_enter(uint32_t ctx) {
    ui_cb_saved_t saved = {.outer_mark = ui_cb_mark_enter(&s_mark, &s_task, ctx)};
    saved.previous      = enter(ctx);
    return saved;
}
static void env_leave(ui_cb_saved_t saved) {
    leave(saved.previous);
    ui_cb_mark_leave(&s_mark, saved.outer_mark);
}
static bool mark_holds(void) { return ui_cb_mark_holds(&s_mark, &s_task, self()); }

static ui_cb_env_t const s_env = {
    .alloc = count_alloc, .free = count_free, .self = self, .enter = env_enter, .leave = env_leave};

static uint32_t ctx_a, ctx_b, seen, seen_inner;
static ui_cb_rec_t *s_inner, *s_self_rec;

static void note_event(struct _lv_event_t *e) {
    (void)e;
    seen = self();
}
static void note_timer(struct _lv_timer_t *t) {
    t->ticks++;
    seen = self();
}
static void note_group(struct _lv_group_t *g) {
    g->moves++;
    seen = self();
}
static void inner_event(struct _lv_event_t *e) {
    (void)e;
    seen_inner = self();
}
// An app's callback that sets off another app's (an event sent on, a hook): the outer context comes back after it
static void outer_event(struct _lv_event_t *e) {
    ui_cb_run_event(&s_env, s_inner, e);
    seen = self();
}
// Deletes its own object, as a key handler that closes the screen: LVGL frees the record during the call
static void deleting_timer(struct _lv_timer_t *t) {
    (void)t;
    seen = self();
    ui_cb_free(&s_env, s_self_rec);
}

static void setup(void) {
    app_ctx_init(&s_t, 0x1234u);
    ctx_a  = app_ctx_add(&s_t, &app_a, 0x55u);
    ctx_b  = app_ctx_add(&s_t, &app_b, 0x77u);
    s_slot = 0;
}

static void test_runs_as_registrant(void) {
    setup();
    uint32_t os = app_ctx_os(&s_t);
    uint32_t prev = enter(ctx_a); // inside app A's open hook
    ui_cb_rec_t *ev = ui_cb_new_event(&s_env, note_event);
    ui_cb_rec_t *tm = ui_cb_new_timer(&s_env, note_timer);
    leave(prev);
    CHECK(ev && tm && s_live == 2);
    CHECK(self() == os); // later, a key press or a tick with nothing entered: the LVGL task runs as the OS
    struct _lv_event_t e = {0};
    struct _lv_timer_t t = {0};
    int allocs = s_allocs;
    ui_cb_run_event(&s_env, ev, &e);
    CHECK(seen == ctx_a);
    CHECK(self() == os);
    seen = 0;
    ui_cb_run_timer(&s_env, tm, &t);
    CHECK(seen == ctx_a && t.ticks == 1);
    CHECK(self() == os);
    CHECK(s_allocs == allocs); // nothing allocated on the call path
    prev            = enter(ctx_b); // MeshCore's chat sets its group's focus callback (#349 review)
    ui_cb_rec_t *gr = ui_cb_new_group(&s_env, note_group);
    leave(prev);
    struct _lv_group_t g = {0};
    ui_cb_run_group(&s_env, gr, &g);
    CHECK(seen == ctx_b && g.moves == 1 && self() == os);
    CHECK(s_allocs == allocs + 1);
    ui_cb_free(&s_env, ev);
    ui_cb_free(&s_env, tm);
    ui_cb_free(&s_env, gr);
    CHECK(s_live == 0);
}

static void test_os_registration_stays_os(void) {
    setup();
    ui_cb_rec_t *ev = ui_cb_new_event(&s_env, note_event); // the shell at boot: nothing entered
    uint32_t prev = enter(ctx_b);                         // fired from inside app B's callback
    struct _lv_event_t e = {0};
    ui_cb_run_event(&s_env, ev, &e);
    CHECK(seen == app_ctx_os(&s_t));
    CHECK(self() == ctx_b);
    leave(prev);
    ui_cb_free(&s_env, ev);
    CHECK(s_live == 0);
}

static void test_nested(void) {
    setup();
    uint32_t prev = enter(ctx_b);
    s_inner = ui_cb_new_event(&s_env, inner_event);
    leave(prev);
    prev = enter(ctx_a);
    ui_cb_rec_t *outer = ui_cb_new_event(&s_env, outer_event);
    leave(prev);
    struct _lv_event_t e = {0};
    ui_cb_run_event(&s_env, outer, &e);
    CHECK(seen_inner == ctx_b);
    CHECK(seen == ctx_a); // restored exactly after the inner one
    CHECK(self() == app_ctx_os(&s_t));
    ui_cb_free(&s_env, outer);
    ui_cb_free(&s_env, s_inner);
    CHECK(s_live == 0);
}

static void test_callback_frees_its_record(void) {
    setup();
    uint32_t prev = enter(ctx_a);
    s_self_rec = ui_cb_new_timer(&s_env, deleting_timer);
    leave(prev);
    struct _lv_timer_t t = {0};
    ui_cb_run_timer(&s_env, s_self_rec, &t); // ASan: no read of the record after the call
    CHECK(seen == ctx_a);
    CHECK(self() == app_ctx_os(&s_t));
    CHECK(s_live == 0);
}

static void test_edges(void) {
    setup();
    CHECK(ui_cb_new_event(&s_env, NULL) == NULL);
    CHECK(ui_cb_new_timer(&s_env, NULL) == NULL);
    CHECK(ui_cb_new_group(&s_env, NULL) == NULL);
    ui_cb_run_group(&s_env, NULL, NULL);
    s_fail_next = 1;
    CHECK(ui_cb_new_event(&s_env, note_event) == NULL);
    CHECK(s_live == 0);
    ui_cb_free(&s_env, NULL);
    struct _lv_event_t e = {0};
    seen = 0xdeadu;
    ui_cb_run_event(&s_env, NULL, &e); // a descriptor LVGL already let go of
    ui_cb_run_timer(&s_env, NULL, NULL);
    CHECK(seen == 0xdeadu);
    // A record whose context went bad (made up, here) runs as the OS and is counted, never as a stale app
    uint32_t refused = s_t.refused;
    ui_cb_rec_t *ev = ui_cb_new_event(&s_env, note_event);
    ev->ctx = ctx_a ^ 0x100u;
    ui_cb_run_event(&s_env, ev, &e);
    CHECK(seen == app_ctx_os(&s_t));
    CHECK(s_t.refused == refused + 1);
    ui_cb_free(&s_env, ev);
    CHECK(s_live == 0);
}

// #427: the panic hook's mark holds only for the task inside a callback while it still runs as the context the
// callback entered; another app's hook run from there (Settings opening an app's settings) strikes that app
static void test_mark_tied_to_context(void) {
    ui_cb_mark_t m    = {0};
    int          task = 0, other = 0;
    uint32_t     a = 0x00012301u, b = 0x00045602u, a_new_gen = 0x00078901u, os = 0x00011100u;
    CHECK(!ui_cb_mark_holds(&m, &task, a)); // nothing marked
    ui_cb_mark_enter(&m, &task, a);
    CHECK(ui_cb_mark_holds(&m, &task, a));
    CHECK(!ui_cb_mark_holds(&m, &task, b));      // a's callback entered b's context: b's crash, a strike
    CHECK(!ui_cb_mark_holds(&m, &other, a));     // another task (the other core) is not in the callback
    CHECK(!ui_cb_mark_holds(&m, &task, os));     // nor is the OS the callback put back
    CHECK(ui_cb_mark_holds(&m, &task, a_new_gen)); // the slot is what is compared

    uint32_t outer = ui_cb_mark_enter(&m, &task, b); // nested: b's callback, sent from inside a's
    CHECK(outer == a && ui_cb_mark_holds(&m, &task, b) && !ui_cb_mark_holds(&m, &task, a));
    ui_cb_mark_leave(&m, outer); // back in a's callback
    CHECK(ui_cb_mark_holds(&m, &task, a) && !ui_cb_mark_holds(&m, &task, b));
    ui_cb_mark_leave(&m, 0);
    CHECK(!ui_cb_mark_holds(&m, &task, a) && !ui_cb_mark_holds(&m, &task, os) && m.task == NULL);
    ui_cb_mark_leave(&m, a); // a leave without an enter changes nothing
    CHECK(m.depth == 0 && m.task == NULL && !ui_cb_mark_holds(&m, &task, a));

    // Half-written at depth 0: ctx is stored before task, so the state in between holds for nobody
    ui_cb_mark_t half = {.task = NULL, .ctx = a, .depth = 0};
    CHECK(!ui_cb_mark_holds(&half, &task, a));
    // An OS callback (slot 0) never holds: the OS is never named, and an app hook it runs strikes that app
    ui_cb_mark_enter(&m, &task, os);
    CHECK(!ui_cb_mark_holds(&m, &task, os) && !ui_cb_mark_holds(&m, &task, a));
    ui_cb_mark_leave(&m, 0);
}

// #427 review: a nested leave puts back the context the outer callback marked, not the task's context before the
// inner enter. Settings' row (an OS callback) enters app X for X's settings hook; X's hook fires one of its own
// callbacks; after it returns, the rest of X's hook is a strike again, so the mark must not hold for X.
static ui_cb_rec_t *s_x_rec, *s_b_rec;
static bool         s_held_inner, s_held_after;
static void x_own_event(struct _lv_event_t *e) {
    (void)e;
    s_held_inner = mark_holds();
}
static void settings_row(struct _lv_event_t *e) { // runs as the OS
    uint32_t prev = enter(ctx_b);                    // build_app_page: X's (here b's) settings hook from here
    ui_cb_run_event(&s_env, s_x_rec, e);             // X's hook sends an event to a row it just made
    s_held_after = mark_holds();                     // the rest of X's hook
    leave(prev);
}
static void a_enters_b(struct _lv_event_t *e) { // app A's callback runs app B's hook
    uint32_t prev = enter(ctx_b);
    ui_cb_run_event(&s_env, s_b_rec, e); // B's own callback, nested
    s_held_after = mark_holds();          // B's hook again: a strike against B
    leave(prev);
    s_held_inner = mark_holds(); // back in A's callback: the mark holds for A
}

static void test_nested_leave_restores_outer_mark(void) {
    setup();
    struct _lv_event_t e = {0};
    uint32_t           prev = enter(ctx_b);
    s_x_rec                 = ui_cb_new_event(&s_env, x_own_event);
    s_b_rec                 = ui_cb_new_event(&s_env, x_own_event);
    leave(prev);
    ui_cb_rec_t *row = ui_cb_new_event(&s_env, settings_row); // the shell's: OS
    s_held_inner = s_held_after = true;
    ui_cb_run_event(&s_env, row, &e);
    CHECK(s_held_inner);  // inside X's own callback: named, no strike
    CHECK(!s_held_after); // X's hook after it: a strike against X
    CHECK(!mark_holds() && s_mark.depth == 0 && s_mark.task == NULL);

    prev              = enter(ctx_a);
    ui_cb_rec_t *a_cb = ui_cb_new_event(&s_env, a_enters_b);
    leave(prev);
    s_held_inner = false;
    s_held_after = true;
    ui_cb_run_event(&s_env, a_cb, &e);
    CHECK(!s_held_after && s_held_inner);
    CHECK(!mark_holds() && s_mark.depth == 0);
    ui_cb_free(&s_env, row);
    ui_cb_free(&s_env, a_cb);
    ui_cb_free(&s_env, s_x_rec);
    ui_cb_free(&s_env, s_b_rec);
    CHECK(s_live == 0);
}

int main(void) {
    test_mark_tied_to_context();
    test_nested_leave_restores_outer_mark();
    test_runs_as_registrant();
    test_os_registration_stays_os();
    test_nested();
    test_callback_frees_its_record();
    test_edges();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ui_cb_test: all passed\n");
    return 0;
}
