// Host test: per-app settings rules (#84 T5): the NVS namespace from an app id, and adopting a legacy key (the plan, and
// the whole run over a fake store: MeshCore's mc_name_set and its advert keys mc_advint/mc_advlast/mc_advfl, #123)
#include "app_ns.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// One key in both namespaces, and which NVS steps fail
typedef struct {
    bool     have_app, have_legacy;
    uint32_t app, legacy;
    bool     fail_set, fail_erase_legacy, corrupt_write; // corrupt_write: the write lands, a different value reads back
    int      sets, erases_app, erases_legacy;
} fake_t;

static bool f_get_app(void *ctx, uint32_t *v) {
    fake_t *f = ctx;
    *v        = f->app;
    return f->have_app;
}
static bool f_get_legacy(void *ctx, uint32_t *v) {
    fake_t *f = ctx;
    *v        = f->legacy;
    return f->have_legacy;
}
static bool f_set_app(void *ctx, uint32_t v) {
    fake_t *f = ctx;
    f->sets++;
    if (f->fail_set) {
        return false;
    }
    f->have_app = true;
    f->app      = f->corrupt_write ? v ^ 1 : v;
    return true;
}
static bool f_erase_app(void *ctx) {
    fake_t *f = ctx;
    f->erases_app++;
    f->have_app = false;
    return true;
}
static bool f_erase_legacy(void *ctx) {
    fake_t *f = ctx;
    f->erases_legacy++;
    if (f->fail_erase_legacy) {
        return false;
    }
    f->have_legacy = false;
    return true;
}

static app_adopted_t run(fake_t *f, uint32_t *legacy) {
    app_adopt_io_t io = {f, f_get_app, f_get_legacy, f_set_app, f_erase_app, f_erase_legacy};
    return app_adopt_run(&io, legacy);
}

static void adopt_runs(void) {
    uint32_t got = 0;
    // Lucas's badge, first boot: mc_advint 720 (12 h) only in badge_fw -> copied as is, the old key kept for a rollback
    fake_t f = {.have_legacy = true, .legacy = 720};
    CHECK(run(&f, &got) == APP_ADOPTED_COPIED && got == 720);
    CHECK(f.have_app && f.app == 720 && f.have_legacy && f.erases_legacy == 0);
    // Second boot: both -> the old key goes, the app's value stays
    CHECK(run(&f, &got) == APP_ADOPTED_DROPPED);
    CHECK(f.have_app && f.app == 720 && !f.have_legacy);
    // Third boot: nothing left to do, nothing written
    int sets = f.sets;
    CHECK(run(&f, &got) == APP_ADOPTED_NOTHING && f.sets == sets && f.app == 720);

    // mc_advlast: a full 32-bit epoch survives the copy
    fake_t last = {.have_legacy = true, .legacy = 0xFEDCBA98u};
    CHECK(run(&last, &got) == APP_ADOPTED_COPIED && last.app == 0xFEDCBA98u);
    // mc_advfl 0 (zero-hop manual advert) is a value too: copied, not mistaken for "missing"
    fake_t fl = {.have_legacy = true, .legacy = 0};
    CHECK(run(&fl, &got) == APP_ADOPTED_COPIED && fl.have_app && fl.app == 0);

    // A fresh badge: no key anywhere, nothing written (the app's default applies, as before)
    fake_t fresh = {0};
    CHECK(run(&fresh, &got) == APP_ADOPTED_NOTHING && fresh.sets == 0 && !fresh.have_app);
    // Already the app's, no old key: untouched
    fake_t own = {.have_app = true, .app = 1440};
    CHECK(run(&own, &got) == APP_ADOPTED_NOTHING && own.app == 1440 && own.sets == 0);
    // Both, different values (the app's was changed after the copy): the app's wins, the old key goes
    fake_t both = {.have_app = true, .app = 1440, .have_legacy = true, .legacy = 720};
    CHECK(run(&both, &got) == APP_ADOPTED_DROPPED && both.app == 1440 && !both.have_legacy);

    // The write fails: the old key stays, the next boot copies
    fake_t bad = {.have_legacy = true, .legacy = 720, .fail_set = true};
    CHECK(run(&bad, &got) == APP_ADOPTED_COPY_FAILED && bad.have_legacy && !bad.have_app);
    bad.fail_set = false;
    CHECK(run(&bad, &got) == APP_ADOPTED_COPIED && bad.app == 720 && bad.have_legacy);
    // A copy that reads back wrong is taken out again, so it can never win over the old key on the next boot
    fake_t wrong = {.have_legacy = true, .legacy = 720, .corrupt_write = true};
    CHECK(run(&wrong, &got) == APP_ADOPTED_COPY_FAILED && !wrong.have_app && wrong.have_legacy && wrong.erases_app == 1);
    // The old key's erase fails: reported, tried again next boot, the app's value untouched
    fake_t stuck = {.have_app = true, .app = 720, .have_legacy = true, .legacy = 720, .fail_erase_legacy = true};
    CHECK(run(&stuck, &got) == APP_ADOPTED_DROP_FAILED && stuck.have_legacy && stuck.app == 720);
    stuck.fail_erase_legacy = false;
    CHECK(run(&stuck, &got) == APP_ADOPTED_DROPPED && !stuck.have_legacy);
}

int main(void) {
    char ns[16];
    CHECK(app_ns_name("meshcore", ns, sizeof(ns)) && strcmp(ns, "a.meshcore") == 0);
    CHECK(app_ns_name("k_1", ns, sizeof(ns)) && strcmp(ns, "a.k_1") == 0);
    CHECK(app_ns_name("abcdefghijklm", ns, sizeof(ns)) && strlen(ns) == 15); // 13 characters: the longest id
    strcpy(ns, "x");
    CHECK(!app_ns_name("abcdefghijklmn", ns, sizeof(ns)) && strcmp(ns, "x") == 0); // 14: too long, out untouched
    CHECK(!app_ns_name("", ns, sizeof(ns)));
    CHECK(!app_ns_name(NULL, ns, sizeof(ns)));
    CHECK(!app_ns_name("Mesh", ns, sizeof(ns)));  // no capitals
    CHECK(!app_ns_name("a-b", ns, sizeof(ns)));   // no dashes
    CHECK(!app_ns_name("a.b", ns, sizeof(ns)));   // no dots
    CHECK(!app_ns_name("meshcore", ns, 15));      // a buffer that cannot hold the longest name

    CHECK(app_adopt_plan(false, true) == APP_ADOPT_COPY);  // only the old key: copy it once
    CHECK(app_adopt_plan(true, true) == APP_ADOPT_DROP_LEGACY); // already adopted: the old key goes, never wins again
    CHECK(app_adopt_plan(true, false) == APP_ADOPT_NONE);
    CHECK(app_adopt_plan(false, false) == APP_ADOPT_NONE); // a fresh badge: nothing to adopt

    adopt_runs();

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
