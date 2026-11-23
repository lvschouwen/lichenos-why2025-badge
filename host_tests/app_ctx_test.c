// The app context table (#230 E1a, app_ctx.c): registration hands out a slot + generation, the OS has slot 0, a made-up,
// stale or out-of-range context is refused and counted, and the refusal log is rate-limited.
#include "app_ctx.h"

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

static app_ctx_table_t *s_made[64];
static size_t           s_made_n;
static void             tinit_with(app_ctx_table_t *t, uint32_t draw, app_ctx_mem_t const *mem, size_t limit,
                                   size_t state_size) {
    app_ctx_init_with(t, draw, mem, limit, state_size);
    s_made[s_made_n++] = t; // destroyed at the end of main: every table's chunks go back
}
static void tinit(app_ctx_table_t *t, uint32_t draw) {
    tinit_with(t, draw, NULL, APP_CTX_MAX_APPS, 0);
}

static int const app_a = 1, app_b = 2, app_c = 3; // stand-ins for lichen_app_t structs: only the address counts

static void test_register(void) {
    static app_ctx_table_t t;
    tinit(&t, 0xabcdef12u);
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
    static app_ctx_table_t t;
    tinit(&t, 0x01000000u); // low 24 bits all 0
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

// The table grows in PSRAM chunks to the limit and refuses the next one; the handle names at most 255 apps
static void test_growth(void) {
    static char apps[APP_CTX_SLOTS + 1];
    static app_ctx_table_t t;
    tinit(&t, 9);
    for (unsigned i = 0; i < APP_CTX_MAX_APPS; i++) { // past 24, past 32, to the encoding's end
        uint32_t ctx = app_ctx_add(&t, &apps[i], i + 100);
        CHECK(ctx != APP_CTX_NONE && (ctx & 0xffu) == i + 1u && app_ctx_valid(&t, ctx));
    }
    CHECK(app_ctx_apps(&t) == APP_CTX_MAX_APPS && APP_CTX_MAX_APPS == 255);
    CHECK(app_ctx_add(&t, &apps[APP_CTX_MAX_APPS], 7) == APP_CTX_NONE); // slot 256 does not fit 8 bits
    for (size_t i = 0; i < APP_CTX_MAX_APPS; i++) {
        CHECK(app_ctx_app_at(&t, i) == &apps[i] && app_ctx_valid(&t, app_ctx_at(&t, i)));
    }
    CHECK(app_ctx_app_at(&t, APP_CTX_MAX_APPS) == NULL);
}

static unsigned s_allocs;
static unsigned s_alloc_fail_after = ~0u;
static void *counting_alloc(size_t size) {
    if (s_allocs >= s_alloc_fail_after) {
        return NULL;
    }
    s_allocs++;
    return calloc(1, size);
}
static void counting_free(void *p) {
    free(p);
}

// The chunks come from the injected allocator, one per 16 slots, and a limit below the encoding's is honoured
static void test_limit_and_allocator(void) {
    static char apps[64];
    app_ctx_mem_t   mem = {.alloc = counting_alloc, .release = counting_free};
    static app_ctx_table_t t;
    s_allocs = 0;
    tinit_with(&t, 5, &mem, 40, 0);
    for (unsigned i = 0; i < 40; i++) {
        CHECK(app_ctx_add(&t, &apps[i], i + 1) != APP_CTX_NONE);
    }
    CHECK(s_allocs == 3); // slots 1..40 are chunks 0..2
    CHECK(app_ctx_add(&t, &apps[40], 1) == APP_CTX_NONE && app_ctx_apps(&t) == 40);
    // out of memory when the next chunk is needed: refused, the table is as it was
    static app_ctx_table_t u;
    s_allocs           = 0;
    s_alloc_fail_after = 1;
    tinit_with(&u, 5, &mem, 40, 0);
    for (unsigned i = 0; i < 15; i++) {
        CHECK(app_ctx_add(&u, &apps[i], i + 1) != APP_CTX_NONE);
    }
    CHECK(app_ctx_add(&u, &apps[15], 1) == APP_CTX_NONE && app_ctx_apps(&u) == 15);
    s_alloc_fail_after = ~0u;
}

static void test_remove_and_reuse(void) {
    static app_ctx_table_t t;
    tinit(&t, 0x303030u);
    uint32_t a = app_ctx_add(&t, &app_a, 0x111111u);
    uint32_t b = app_ctx_add(&t, &app_b, 0x222222u);
    uint32_t c = app_ctx_add(&t, &app_c, 0x333333u);
    void const *owner = NULL;
    CHECK(!app_ctx_remove(&t, app_ctx_os(&t)) && !app_ctx_remove(&t, APP_CTX_NONE) && !app_ctx_remove(&t, b ^ 0x100u));
    CHECK(app_ctx_apps(&t) == 3);
    CHECK(app_ctx_remove(&t, b));
    // the stale handle is refused (counted), its slot has no owner, the others keep their contexts
    CHECK(!app_ctx_valid(&t, b) && !app_ctx_resolve(&t, b, &owner) && t.refused == 1);
    CHECK(app_ctx_slot_owner(&t, b) == NULL);
    CHECK(!app_ctx_remove(&t, b)); // twice
    CHECK(app_ctx_valid(&t, a) && app_ctx_valid(&t, c));
    // the dense view closes the gap: c moved from index 2 to 1 but its ctx did not change
    CHECK(app_ctx_apps(&t) == 2 && app_ctx_app_at(&t, 0) == &app_a && app_ctx_app_at(&t, 1) == &app_c);
    CHECK(app_ctx_app_at(&t, 2) == NULL && app_ctx_at(&t, 1) == c && app_ctx_at(&t, 2) == APP_CTX_NONE);
    CHECK(app_ctx_of(&t, &app_b) == APP_CTX_NONE && app_ctx_of(&t, &app_c) == c);
    // re-register: the freed slot comes back, appended last, with another generation than b had even for the same draw
    uint32_t b2 = app_ctx_add(&t, &app_b, 0x222222u);
    CHECK(b2 != APP_CTX_NONE && (b2 & 0xffu) == (b & 0xffu) && b2 != b && !app_ctx_valid(&t, b) && app_ctx_valid(&t, b2));
    CHECK(app_ctx_app_at(&t, 2) == &app_b && app_ctx_at(&t, 2) == b2);
    CHECK(app_ctx_resolve(&t, b2, &owner) && owner == &app_b);
    // removing the first app shifts everything up
    CHECK(app_ctx_remove(&t, a) && app_ctx_app_at(&t, 0) == &app_c && app_ctx_app_at(&t, 1) == &app_b);
    CHECK(app_ctx_valid(&t, c) && app_ctx_valid(&t, b2));
}

// A table that was full takes an app again after a remove
static void test_full_then_remove(void) {
    static char apps[APP_CTX_SLOTS];
    static app_ctx_table_t t;
    tinit_with(&t, 9, NULL, 24, 0);
    uint32_t last = 0;
    for (unsigned i = 0; i < 24; i++) {
        last = app_ctx_add(&t, &apps[i], i + 100);
    }
    CHECK(app_ctx_add(&t, &apps[24], 1) == APP_CTX_NONE);
    CHECK(app_ctx_remove(&t, last) && app_ctx_add(&t, &apps[24], 1) != APP_CTX_NONE && app_ctx_apps(&t) == 24);
}

static unsigned s_blocks; // live blocks of the counting allocator below
static void *block_alloc(size_t size) {
    s_blocks++;
    return calloc(1, size);
}
static void block_free(void *p) {
    s_blocks--;
    free(p);
}

// Each app has its own zeroed block, made with the slot and freed with it (#520)
static void test_state(void) {
    static char apps[APP_CTX_SLOTS];
    app_ctx_mem_t   mem = {.alloc = block_alloc, .release = block_free};
    static app_ctx_table_t t;
    s_blocks = 0;
    tinit_with(&t, 0x515151u, &mem, APP_CTX_MAX_APPS, 24);
    uint32_t a = app_ctx_add(&t, &apps[0], 0x111111u);
    uint32_t b = app_ctx_add(&t, &apps[1], 0x222222u);
    unsigned char *sa = app_ctx_state(&t, a), *sb = app_ctx_state(&t, b);
    CHECK(sa && sb && sa != sb);
    if (!sa || !sb) {
        return;
    }
    for (int i = 0; i < 24; i++) {
        CHECK(sa[i] == 0 && sb[i] == 0); // zeroed
    }
    sa[0] = 7;
    CHECK(app_ctx_state(&t, app_ctx_os(&t)) == NULL && app_ctx_state(&t, APP_CTX_NONE) == NULL); // the OS has none
    CHECK(app_ctx_state(&t, a ^ 0x100u) == NULL);                                             // a wrong generation
    unsigned before = s_blocks; // chunks 0 and the blocks of a and b
    CHECK(app_ctx_remove(&t, a));
    CHECK(s_blocks == before - 1);              // the app's block is freed with the app
    CHECK(app_ctx_state(&t, a) == NULL && app_ctx_state(&t, b) == sb); // the stale ctx finds none, b is untouched
    uint32_t a2 = app_ctx_add(&t, &apps[0], 0x111111u);
    unsigned char *sa2 = app_ctx_state(&t, a2);
    CHECK(sa2 && s_blocks == before);
    CHECK(sa2 && sa2[0] == 0); // a re-registered app starts from zero
    // growth: past 48 and to 255, each with its block, all freed again by destroy
    for (unsigned i = 3; i <= APP_CTX_MAX_APPS; i++) {
        uint32_t c = app_ctx_add(&t, &apps[i], i);
        CHECK(c != APP_CTX_NONE && app_ctx_state(&t, c) != NULL);
    }
    CHECK(app_ctx_apps(&t) == APP_CTX_MAX_APPS);
    app_ctx_destroy(&t);
    CHECK(s_blocks == 0);
}

// A block that cannot be had refuses the add and leaves the table as it was
static void test_state_no_memory(void) {
    static app_ctx_table_t t;
    app_ctx_mem_t   mem = {.alloc = counting_alloc, .release = counting_free};
    s_allocs           = 0;
    s_alloc_fail_after = 1; // the first chunk only
    tinit_with(&t, 3, &mem, 40, 16);
    CHECK(app_ctx_add(&t, &app_a, 1) == APP_CTX_NONE && app_ctx_apps(&t) == 0);
    s_alloc_fail_after = ~0u;
    CHECK(app_ctx_add(&t, &app_a, 1) != APP_CTX_NONE && app_ctx_apps(&t) == 1);
}

static char const *name_id(void const *owner) {
    return owner == &app_a ? "alpha" : owner == &app_b ? "beta" : "gamma";
}
static void names_of(void const *owner, char const **id, char const **alias) {
    *id    = name_id(owner);
    *alias = NULL;
}

// An id is taken only while its app is registered: after a remove it is free again
static void test_id_collision_after_remove(void) {
    static app_ctx_table_t t;
    char const     *why = NULL;
    tinit(&t, 0x404040u);
    uint32_t a = app_ctx_add_app(&t, &app_a, 0x111111u, names_of, &why);
    CHECK(a != APP_CTX_NONE && why == NULL);
    CHECK(app_ctx_add_app(&t, &app_a, 0x222222u, names_of, &why) == APP_CTX_NONE && why != NULL); // same id
    CHECK(app_ctx_remove(&t, a));
    why = (char const *)1;
    CHECK(app_ctx_add_app(&t, &app_a, 0x333333u, names_of, &why) != APP_CTX_NONE && why == NULL);
}

static void test_refused(void) {
    static app_ctx_table_t t;
    tinit(&t, 0x111111u);
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
    static app_ctx_table_t one, two;
    tinit(&one, 1);
    tinit(&two, 1);
    uint32_t a1 = app_ctx_add(&one, &app_a, 0x5555u);
    uint32_t a2 = app_ctx_add(&two, &app_a, 0x6666u);
    CHECK(app_ctx_valid(&one, a1) && !app_ctx_valid(&two, a1) && app_ctx_valid(&two, a2));
}

static void test_log_rate(void) {
    static app_ctx_table_t t;
    tinit(&t, 3);
    uint32_t skipped = 99;
    CHECK(app_ctx_log_due(&t, 1000, &skipped) && skipped == 0); // the first one is logged
    CHECK(!app_ctx_log_due(&t, 1001, &skipped));
    CHECK(!app_ctx_log_due(&t, 1000 + APP_CTX_LOG_GAP_MS - 1, &skipped));
    CHECK(app_ctx_log_due(&t, 1000 + APP_CTX_LOG_GAP_MS, &skipped) && skipped == 2); // says how many it held back
    CHECK(!app_ctx_log_due(&t, 1000 + APP_CTX_LOG_GAP_MS + 5, &skipped));
    // the millisecond clock wraps: still one gap
    static app_ctx_table_t w;
    tinit(&w, 3);
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
    static app_ctx_table_t t;
    tinit(&t, 0x404040u);
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
    static app_ctx_table_t t;
    tinit(&t, 0x505050u);
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
    static app_ctx_table_t t;
    tinit(&t, 0x606060u);
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
    static app_ctx_table_t t;
    tinit(&t, 0x707070u);
    uint32_t a  = app_ctx_add(&t, &app_a, 0x5555u);
    uint32_t b  = app_ctx_add(&t, &app_b, 0x6666u);
    uint32_t os = app_ctx_os(&t);
    s_slot      = 0;
    bool unregistered = true;
    CHECK(app_ctx_for_task(&t, &tls, NULL, &unregistered) == os && !unregistered); // the OS starts an OS task
    CHECK(app_ctx_for_task(&t, &tls, &app_b, NULL) == b);
    s_slot = a; // an app's task (or hook) starts one
    CHECK(app_ctx_for_task(&t, &tls, NULL, NULL) == a);   // NULL inherits the app
    CHECK(app_ctx_for_task(&t, &tls, &app_b, NULL) == b); // a named owner wins (lichen_task_start with an app)
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
    static app_ctx_table_t t;
    tinit(&t, 0x808080u);
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
    static app_ctx_table_t t;
    tinit(&t, 0x909090u);
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

// #340: an app's id and alias against every earlier app's id and alias; a refused app takes no slot
typedef struct {
    char const *id;
    char const *alias;
} fake_app_t;

static void fake_names(void const *owner, char const **id, char const **alias) {
    fake_app_t const *app = owner;
    *id                   = app->id;
    *alias                = app->alias;
}

static void test_add_app_refuses_taken_names(void) {
    static fake_app_t const mesh = {"meshcore", "mesh"}, clock = {"clock", NULL}, by_id = {"meshcore", NULL},
                            by_alias = {"mesh", NULL}, alias_clash = {"weather", "clock"}, mine = {"weather", "wx"},
                            no_id = {NULL, NULL}, os_id = {"lichen", NULL};
    static app_ctx_table_t t;
    tinit(&t, 0x123456u);
    char const *why = "x";
    CHECK(app_ctx_add_app(&t, &mesh, 0x11u, fake_names, &why) != APP_CTX_NONE && why == NULL);
    CHECK(app_ctx_add_app(&t, &clock, 0x22u, fake_names, &why) != APP_CTX_NONE && why == NULL);
    fake_app_t const *refused[] = {&by_id, &by_alias, &alias_clash, &no_id, &os_id};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        why = NULL;
        CHECK(app_ctx_add_app(&t, refused[i], 0x33u, fake_names, &why) == APP_CTX_NONE && why != NULL);
        CHECK(app_ctx_apps(&t) == 2 && app_ctx_of(&t, refused[i]) == APP_CTX_NONE); // the table is untouched
    }
    uint32_t w = app_ctx_add_app(&t, &mine, 0x44u, fake_names, &why);
    CHECK(w != APP_CTX_NONE && why == NULL && app_ctx_apps(&t) == 3 && app_ctx_app_at(&t, 2) == &mine);
    CHECK((w & 0xffu) == 3u); // the next slot: the refused apps took none
    why = "x";
    CHECK(app_ctx_add_app(&t, NULL, 0x55u, fake_names, &why) == APP_CTX_NONE && why == NULL);
}

int main(void) {
    test_add_app_refuses_taken_names();
    test_register();
    test_zero_draw_never_gives_ctx_zero();
    test_not_ready();
    test_growth();
    test_limit_and_allocator();
    test_remove_and_reuse();
    test_state();
    test_state_no_memory();
    test_full_then_remove();
    test_id_collision_after_remove();
    test_refused();
    test_other_boot();
    test_log_rate();
    test_enter_leave_nest();
    test_enter_refused_falls_back_to_os();
    test_isr();
    test_task_inherit();
    test_no_tls_paths_are_the_os();
    test_trampoline_sets_and_nests();
    for (size_t i = 0; i < s_made_n; i++) {
        app_ctx_destroy(s_made[i]);
    }
    if (failures) {
        printf("app_ctx_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("app_ctx_test: all passed\n");
    return 0;
}
