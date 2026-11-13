// The app context table (#230 E1a, app_ctx.c): registration hands out a slot + generation, the OS has slot 0, a made-up,
// stale or out-of-range context is refused and counted, and the refusal log is rate-limited.
#include "app_ctx.h"

#include <stdio.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static int const app_a = 1, app_b = 2, app_c = 3; // stand-ins for lichen_app_t structs: only the address counts

static void test_register(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0xabcdef12u);
    CHECK(app_ctx_ready(&t));
    uint32_t os = app_ctx_os(&t);
    CHECK(os == ((0xcdef12u << 8) | 0u)); // slot 0, the draw's low 24 bits
    uint32_t a = app_ctx_add(&t, &app_a, 0x00123456u);
    uint32_t b = app_ctx_add(&t, &app_b, 0x00654321u);
    CHECK(a == ((0x123456u << 8) | 1u) && b == ((0x654321u << 8) | 2u));
    CHECK(app_ctx_apps(&t) == 2);
    CHECK(app_ctx_app_at(&t, 0) == &app_a && app_ctx_app_at(&t, 1) == &app_b && app_ctx_app_at(&t, 2) == NULL);
    CHECK(app_ctx_at(&t, 1) == b && app_ctx_at(&t, 2) == APP_CTX_NONE);
    CHECK(app_ctx_of(&t, &app_a) == a && app_ctx_of(&t, NULL) == os); // lichen_task_start's owner lookup
    CHECK(app_ctx_of(&t, &app_c) == APP_CTX_NONE);                     // never registered
    CHECK(app_ctx_add(&t, NULL, 1) == APP_CTX_NONE);
    CHECK(app_ctx_apps(&t) == 2);
}

static void test_zero_draw_never_gives_ctx_zero(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x01000000u); // low 24 bits all 0
    CHECK(app_ctx_os(&t) != APP_CTX_NONE && app_ctx_valid(&t, app_ctx_os(&t)));
    uint32_t a = app_ctx_add(&t, &app_a, 0);
    CHECK(a != APP_CTX_NONE && (a >> 8) != 0 && app_ctx_valid(&t, a));
}

static void test_not_ready(void) {
    app_ctx_table_t t = {0};
    CHECK(!app_ctx_ready(&t) && app_ctx_os(&t) == APP_CTX_NONE);
    CHECK(app_ctx_add(&t, &app_a, 5) == APP_CTX_NONE);
    CHECK(!app_ctx_valid(&t, 0));
}

static void test_full(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 9);
    static char apps[APP_CTX_SLOTS];
    for (unsigned i = 0; i + 1 < APP_CTX_SLOTS; i++) {
        CHECK(app_ctx_add(&t, &apps[i], i + 100) != APP_CTX_NONE);
    }
    CHECK(app_ctx_add(&t, &apps[APP_CTX_SLOTS - 1], 7) == APP_CTX_NONE); // the OS plus LICHEN_MAX_APPS
    CHECK(app_ctx_apps(&t) == APP_CTX_SLOTS - 1);
}

static void test_refused(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x111111u);
    uint32_t    a     = app_ctx_add(&t, &app_a, 0x222222u);
    void const *owner = &app_c;
    CHECK(app_ctx_resolve(&t, a, &owner) && owner == &app_a);
    CHECK(app_ctx_resolve(&t, app_ctx_os(&t), &owner) && owner == NULL);
    CHECK(t.refused == 0);
    uint32_t const bad[] = {
        APP_CTX_NONE,
        a ^ (1u << 8),             // right slot, wrong generation (a stale or guessed one)
        (0x222222u << 8) | 2u,     // a's generation on a slot nobody took
        (0x222222u << 8) | 200u,   // past the table
        0x222222u,                 // a pointer-looking value
        (uint32_t)(uintptr_t)&app_a,
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        owner = &app_c;
        CHECK(!app_ctx_valid(&t, bad[i]));
        CHECK(!app_ctx_resolve(&t, bad[i], &owner) && owner == &app_c); // untouched
    }
    CHECK(t.refused == sizeof(bad) / sizeof(bad[0]));
    CHECK(t.last_refused == (uint32_t)(uintptr_t)&app_a);
    app_ctx_count_refusal(&t, 42);
    CHECK(t.refused == sizeof(bad) / sizeof(bad[0]) + 1 && t.last_refused == 42);
}

// Two tables (two boots) draw different generations: yesterday's context is refused today
static void test_other_boot(void) {
    app_ctx_table_t one, two;
    app_ctx_init(&one, 1);
    app_ctx_init(&two, 1);
    uint32_t a1 = app_ctx_add(&one, &app_a, 0x5555u);
    uint32_t a2 = app_ctx_add(&two, &app_a, 0x6666u);
    CHECK(app_ctx_valid(&one, a1) && !app_ctx_valid(&two, a1) && app_ctx_valid(&two, a2));
}

static void test_log_rate(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 3);
    uint32_t skipped = 99;
    CHECK(app_ctx_log_due(&t, 1000, &skipped) && skipped == 0); // the first one is logged
    CHECK(!app_ctx_log_due(&t, 1001, &skipped));
    CHECK(!app_ctx_log_due(&t, 1000 + APP_CTX_LOG_GAP_MS - 1, &skipped));
    CHECK(app_ctx_log_due(&t, 1000 + APP_CTX_LOG_GAP_MS, &skipped) && skipped == 2); // says how many it held back
    CHECK(!app_ctx_log_due(&t, 1000 + APP_CTX_LOG_GAP_MS + 5, &skipped));
    // the millisecond clock wraps: still one gap
    app_ctx_table_t w;
    app_ctx_init(&w, 3);
    CHECK(app_ctx_log_due(&w, 0xfffffff0u, &skipped));
    CHECK(!app_ctx_log_due(&w, 0x10u, &skipped));
    CHECK(app_ctx_log_due(&w, APP_CTX_LOG_GAP_MS, &skipped) && skipped == 1);
}


// ---- the current context, over a fake thread-local slot ----

static uint32_t s_slot;  // the task's slot
static bool     s_isr;   // "in an interrupt"
static uint32_t fake_get(void) {
    return s_slot;
}
static void fake_set(uint32_t ctx) {
    s_slot = ctx;
}
static bool fake_isr(void) {
    return s_isr;
}
static app_ctx_tls_t const tls = {.get = fake_get, .set = fake_set, .in_isr = fake_isr};

static void test_enter_leave_nest(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x404040u);
    uint32_t a = app_ctx_add(&t, &app_a, 0x1111u);
    uint32_t b = app_ctx_add(&t, &app_b, 0x2222u);
    uint32_t os = app_ctx_os(&t);
    s_slot = 0;
    s_isr  = false;
    CHECK(app_ctx_self(&t, &tls) == os); // nothing entered: the OS
    bool     refused = true;
    uint32_t outer   = app_ctx_enter(&t, &tls, a, &refused);
    CHECK(!refused && outer == 0 && app_ctx_self(&t, &tls) == a);
    uint32_t inner = app_ctx_enter(&t, &tls, b, NULL); // a's hook opens b's page
    CHECK(inner == a && app_ctx_self(&t, &tls) == b);
    app_ctx_leave(&tls, inner);
    CHECK(app_ctx_self(&t, &tls) == a); // back to a, not to the OS
    app_ctx_leave(&tls, outer);
    CHECK(app_ctx_self(&t, &tls) == os && s_slot == 0);
    CHECK(t.refused == 0);
}

static void test_enter_refused_falls_back_to_os(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x505050u);
    uint32_t a  = app_ctx_add(&t, &app_a, 0x3333u);
    uint32_t os = app_ctx_os(&t);
    s_slot      = 0;
    s_isr       = false;
    uint32_t outer = app_ctx_enter(&t, &tls, a, NULL);
    bool     refused = false;
    uint32_t prev    = app_ctx_enter(&t, &tls, a ^ 0x100u, &refused); // a made-up generation
    CHECK(refused && prev == a && app_ctx_self(&t, &tls) == os && t.refused == 1);
    app_ctx_leave(&tls, prev);
    CHECK(app_ctx_self(&t, &tls) == a);
    app_ctx_leave(&tls, outer);
    CHECK(app_ctx_slot_owner(&t, a ^ 0x100u) == &app_a); // what it claimed to be, for the log
    CHECK(app_ctx_slot_owner(&t, os) == NULL && app_ctx_slot_owner(&t, (5u << 8) | 9u) == NULL);
}

static void test_isr(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x606060u);
    uint32_t a = app_ctx_add(&t, &app_a, 0x4444u);
    s_slot     = a; // the interrupted task runs for a
    s_isr      = true;
    CHECK(app_ctx_self(&t, &tls) == app_ctx_os(&t)); // an ISR has no slot: the OS
    bool refused = true;
    CHECK(app_ctx_enter(&t, &tls, app_ctx_os(&t), &refused) == 0 && !refused && s_slot == a); // nothing changes
    app_ctx_leave(&tls, 0);
    CHECK(s_slot == a);
    s_isr = false;
}

static void test_task_inherit(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x707070u);
    uint32_t a  = app_ctx_add(&t, &app_a, 0x5555u);
    uint32_t b  = app_ctx_add(&t, &app_b, 0x6666u);
    uint32_t os = app_ctx_os(&t);
    s_slot      = 0;
    bool unregistered = true;
    CHECK(app_ctx_for_task(&t, &tls, NULL, &unregistered) == os && !unregistered); // the OS starts an OS task
    CHECK(app_ctx_for_task(&t, &tls, &app_b, NULL) == b);
    s_slot = a; // an app's task (or hook) starts one
    CHECK(app_ctx_for_task(&t, &tls, NULL, NULL) == a);   // NULL inherits the app
    CHECK(app_ctx_for_task(&t, &tls, &app_b, NULL) == b); // a named owner wins (the compat shim, until E1e)
    CHECK(app_ctx_for_task(&t, &tls, &app_c, &unregistered) == os && unregistered); // never registered: the OS
    s_slot = 0;
}

// ---- #230 E1b: the paths that have no thread-local slot of their own, and lichen_task_start's trampoline ----

// Two FreeRTOS tasks over the fake slot: switching saves one task's slot and loads the other's
static uint32_t s_saved;
static void switch_task(void) {
    uint32_t other = s_saved;
    s_saved        = s_slot;
    s_slot         = other;
}

// What lichen_task.c does: lichen_task_start(ctx, ...) is given the caller's ctx (an OS caller passes
// lichen_ctx_self()), books it, and the new task's trampoline enters it before fn runs. A new task's slot starts at 0.
static uint32_t start_and_enter(app_ctx_table_t *t, uint32_t given) {
    if (!app_ctx_resolve(t, given, NULL)) {
        return APP_CTX_NONE; // refused: lichen_task_start returns false, nothing runs
    }
    switch_task();
    s_slot = 0;
    (void)app_ctx_enter(t, &tls, given, NULL); // the trampoline, for good
    return app_ctx_self(t, &tls);
}

static void test_no_tls_paths_are_the_os(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x808080u);
    uint32_t a  = app_ctx_add(&t, &app_a, 0x7777u);
    uint32_t os = app_ctx_os(&t);
    s_isr       = false;
    s_slot      = 0;
    s_saved     = 0;
    uint32_t prev = app_ctx_enter(&t, &tls, a, NULL); // a's hook runs on the LVGL task
    // ... and arms an esp_timer: its callback runs on the esp_timer task, which never entered anything
    switch_task();
    CHECK(app_ctx_self(&t, &tls) == os);
    switch_task();
    CHECK(app_ctx_self(&t, &tls) == a); // the hook's task is still a
    // An interrupt inside the hook: no slot of its own, so the OS, whatever the interrupted task entered
    s_isr = true;
    CHECK(app_ctx_self(&t, &tls) == os);
    CHECK(app_ctx_enter(&t, &tls, a, NULL) == 0 && s_slot == a); // and it cannot switch the task's slot
    s_isr = false;
    app_ctx_leave(&tls, prev);
    CHECK(app_ctx_self(&t, &tls) == os && t.refused == 0);
}

static void test_trampoline_sets_and_nests(void) {
    app_ctx_table_t t;
    app_ctx_init(&t, 0x909090u);
    uint32_t a  = app_ctx_add(&t, &app_a, 0x8888u);
    uint32_t b  = app_ctx_add(&t, &app_b, 0x9999u);
    uint32_t os = app_ctx_os(&t);
    s_isr       = false;
    s_slot      = 0;
    s_saved     = 0;
    // Boot: main starts an OS service task with its own (the OS's) ctx
    CHECK(start_and_enter(&t, app_ctx_self(&t, &tls)) == os);
    // a's start_background hook starts a's loop task with lichen_ctx_self(): the task runs as a
    s_slot = 0;
    (void)app_ctx_enter(&t, &tls, a, NULL);
    CHECK(start_and_enter(&t, app_ctx_self(&t, &tls)) == a);
    // a's loop task starts a job the same way: nested tasks inherit a
    CHECK(start_and_enter(&t, app_ctx_self(&t, &tls)) == a);
    // An app task naming another app's ctx (the shell starting b's task from a's hook) runs as b
    CHECK(start_and_enter(&t, b) == b);
    // A stale or made-up ctx starts nothing and is counted
    uint32_t refused_before = t.refused;
    CHECK(start_and_enter(&t, a ^ 0x100u) == APP_CTX_NONE && t.refused == refused_before + 1);
    CHECK(start_and_enter(&t, APP_CTX_NONE) == APP_CTX_NONE && t.refused == refused_before + 2);
    s_slot  = 0;
    s_saved = 0;
}

int main(void) {
    test_register();
    test_zero_draw_never_gives_ctx_zero();
    test_not_ready();
    test_full();
    test_refused();
    test_other_boot();
    test_log_rate();
    test_enter_leave_nest();
    test_enter_refused_falls_back_to_os();
    test_isr();
    test_task_inherit();
    test_no_tls_paths_are_the_os();
    test_trampoline_sets_and_nests();
    if (failures) {
        printf("app_ctx_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("app_ctx_test: all passed\n");
    return 0;
}
