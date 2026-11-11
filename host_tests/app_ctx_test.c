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
    CHECK(app_ctx_of(&t, &app_a) == a && app_ctx_of(&t, NULL) == os); // the compat shim's lookup
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

int main(void) {
    test_register();
    test_zero_draw_never_gives_ctx_zero();
    test_not_ready();
    test_full();
    test_refused();
    test_other_boot();
    test_log_rate();
    if (failures) {
        printf("app_ctx_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("app_ctx_test: all passed\n");
    return 0;
}
