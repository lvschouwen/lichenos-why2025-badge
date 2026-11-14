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

static ui_cb_env_t const s_env = {
    .alloc = count_alloc, .free = count_free, .self = self, .enter = enter, .leave = leave};

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
    ui_cb_free(&s_env, ev);
    ui_cb_free(&s_env, tm);
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

int main(void) {
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
