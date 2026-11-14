// The run-time capability check (#229, app_caps.c): a call that needs a capability the app did not declare is
// refused and counted, the OS and SYSTEM apps always pass, a ctx that is not valid is left to the call's own check,
// and the refusal log is rate-limited.
#include "app_caps.h"
#include "lichen_caps.h"

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

// Stand-ins for lichen_app_t: only the caps member counts
typedef struct {
    uint32_t caps;
} fake_app_t;

static fake_app_t const timer   = {.caps = CAP_NOTIFY};
static fake_app_t const keytest = {.caps = CAP_RAW_KEYS};
static fake_app_t const shell   = {.caps = CAP_SYSTEM};
static fake_app_t const mesh    = {.caps = CAP_LORA_TX | CAP_LORA_RX | CAP_SD | CAP_BACKGROUND | CAP_NOTIFY};

static uint32_t caps_of(void const *owner) {
    return ((fake_app_t const *)owner)->caps;
}

typedef struct {
    app_ctx_table_t     t;
    app_caps_refusals_t r;
    uint32_t            os, timer, keytest, shell, mesh;
} world_t;

static void setup(world_t *w) {
    memset(w, 0, sizeof(*w));
    app_ctx_init(&w->t, 0x11111111u);
    w->os      = app_ctx_os(&w->t);
    w->timer   = app_ctx_add(&w->t, &timer, 0x222222u);
    w->keytest = app_ctx_add(&w->t, &keytest, 0x333333u);
    w->shell   = app_ctx_add(&w->t, &shell, 0x444444u);
    w->mesh    = app_ctx_add(&w->t, &mesh, 0x555555u);
}

static void test_allows(void) {
    CHECK(app_caps_allows(CAP_SD | CAP_NOTIFY, CAP_SD));
    CHECK(app_caps_allows(CAP_SD | CAP_NOTIFY, CAP_SD | CAP_NOTIFY));
    CHECK(!app_caps_allows(CAP_SD, CAP_SD | CAP_NOTIFY)); // every bit of need
    CHECK(!app_caps_allows(0, CAP_BACKGROUND));           // main.c: start_background without BACKGROUND is skipped
    CHECK(app_caps_allows(CAP_SYSTEM, CAP_RAW_KEYS | CAP_SECRETS));
    CHECK(app_caps_allows(0, 0));
}

static void test_undeclared_cap_refuses(void) {
    world_t w;
    setup(&w);
    CHECK(!app_caps_check(&w.r, &w.t, w.timer, CAP_RAW_KEYS, caps_of));
    CHECK(!app_caps_check(&w.r, &w.t, w.keytest, CAP_NOTIFY, caps_of));
    CHECK(w.r.refused == 2 && w.r.last_need == CAP_NOTIFY);
    CHECK(w.r.per_slot[1] == 1 && w.r.per_slot[2] == 1 && w.r.per_slot[3] == 0);
}

static void test_declared_os_and_system_pass(void) {
    world_t w;
    setup(&w);
    CHECK(app_caps_check(&w.r, &w.t, w.timer, CAP_NOTIFY, caps_of));
    CHECK(app_caps_check(&w.r, &w.t, w.keytest, CAP_RAW_KEYS, caps_of));
    CHECK(app_caps_check(&w.r, &w.t, w.mesh, CAP_SD, caps_of));
    CHECK(app_caps_check(&w.r, &w.t, w.os, CAP_SECRETS, caps_of)); // the OS (ISRs, esp_timer, LVGL callbacks)
    CHECK(app_caps_check(&w.r, &w.t, w.shell, CAP_SECRETS | CAP_RAW_KEYS, caps_of));
    CHECK(app_caps_check(&w.r, &w.t, w.timer, 0, caps_of)); // an ungated call
    CHECK(w.r.refused == 0);
}

static void test_invalid_ctx_is_left_to_the_ctx_check(void) {
    world_t w;
    setup(&w);
    uint32_t stale = (w.timer & 0xffu) | (0x999999u << 8);
    CHECK(app_caps_check(&w.r, &w.t, stale, CAP_RAW_KEYS, caps_of));
    CHECK(app_caps_check(&w.r, &w.t, 0x77u, CAP_RAW_KEYS, caps_of)); // a slot no app has
    CHECK(app_caps_check(&w.r, &w.t, APP_CTX_NONE, CAP_RAW_KEYS, caps_of));
    CHECK(w.r.refused == 0);
}

static void test_log_rate_limit(void) {
    world_t  w;
    uint32_t skipped = 99;
    setup(&w);
    CHECK(app_caps_log_due(&w.r, 0, &skipped) && skipped == 0); // the first one, even at time 0
    CHECK(!app_caps_log_due(&w.r, 10, &skipped));
    CHECK(!app_caps_log_due(&w.r, APP_CTX_LOG_GAP_MS - 1, &skipped));
    CHECK(app_caps_log_due(&w.r, APP_CTX_LOG_GAP_MS, &skipped) && skipped == 2);
    CHECK(!app_caps_log_due(&w.r, APP_CTX_LOG_GAP_MS + 5, &skipped));
    CHECK(app_caps_log_due(&w.r, 0xfffffff0u, &skipped) && skipped == 1);
    CHECK(app_caps_log_due(&w.r, 0xfffffff0u + APP_CTX_LOG_GAP_MS, &skipped) && skipped == 0); // across the wrap
}

static void test_names_and_format(void) {
    char out[64];
    CHECK(strcmp(app_caps_name(0), "LORA_TX") == 0 && strcmp(app_caps_name(9), "BACKGROUND") == 0);
    CHECK(strcmp(app_caps_name(CAP_COUNT - 1), "SYSTEM") == 0 && app_caps_name(CAP_COUNT) == NULL);
    CHECK(app_caps_format(0, out, sizeof(out)) == 1 && strcmp(out, "-") == 0);
    app_caps_format(CAP_SD | CAP_NOTIFY | CAP_BACKGROUND, out, sizeof(out));
    CHECK(strcmp(out, "SD BACKGROUND NOTIFY") == 0);
    char small[8];
    size_t n = app_caps_format(CAP_LORA_TX | CAP_LORA_RX, small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small) && strncmp(small, "LORA_TX", 7) == 0);
    CHECK(app_caps_format(CAP_SD, out, 0) == 0);
}

int main(void) {
    test_allows();
    test_undeclared_cap_refuses();
    test_declared_os_and_system_pass();
    test_invalid_ctx_is_left_to_the_ctx_check();
    test_log_rate_limit();
    test_names_and_format();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("caps_test: ok\n");
    return 0;
}
