// Host test: the SD apps' loader rules (#523): scan verdicts and reasons, whether an open may load, the load/unload
// state machine and the unload order.
#include "sd_app_rules.h"

#include "lichen_api_version.h"

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

static lichen_manifest_t good(void) {
    lichen_manifest_t m;
    memset(&m, 0, sizeof(m));
    strcpy(m.id, "hello");
    strcpy(m.name, "Hello");
    strcpy(m.entry, "app.elf");
    m.api_major = LICHEN_API_VERSION_MAJOR;
    m.api_minor = LICHEN_API_VERSION_MINOR;
    return m;
}

static void test_scan(void) {
    lichen_manifest_t m = good();
    char              why[96];
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, false, true) == SD_APP_SCAN_OK);
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_ERR_KEY, &m, false, true) == SD_APP_SCAN_BAD_MANIFEST);
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, NULL, false, true) == SD_APP_SCAN_BAD_MANIFEST);
    m.api_major = LICHEN_API_VERSION_MAJOR + 1;
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, false, true) == SD_APP_SCAN_API);
    m = good();
    m.api_minor = LICHEN_API_VERSION_MINOR + 1; // newer than this OS
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, false, true) == SD_APP_SCAN_API);
    m = good();
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, true, true) == SD_APP_SCAN_SHADOW); // a built-in's id or alias
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, false, false) == SD_APP_SCAN_NO_ENTRY);
    m = good();
    m.stack_kib = SD_APP_STACK_KIB_MAX;
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, false, true) == SD_APP_SCAN_OK);
    m.stack_kib = SD_APP_STACK_KIB_MAX + 1;
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, false, true) == SD_APP_SCAN_STACK);
    sd_app_scan_reason(SD_APP_SCAN_STACK, LICHEN_MANIFEST_OK, &m, "hello", why, sizeof(why));
    CHECK(strstr(why, "17 KiB") && strstr(why, "16"));
    m = good();
    // the order: an API refusal names the API even when the id is also taken
    m.api_major = 9;
    CHECK(sd_app_scan_verdict(LICHEN_MANIFEST_OK, &m, true, false) == SD_APP_SCAN_API);

    m = good();
    sd_app_scan_reason(SD_APP_SCAN_SHADOW, LICHEN_MANIFEST_OK, &m, "hello", why, sizeof(why));
    CHECK(strstr(why, "hello") && strstr(why, "already"));
    m.api_major = 3;
    m.api_minor = 0;
    sd_app_scan_reason(SD_APP_SCAN_API, LICHEN_MANIFEST_OK, &m, "hello", why, sizeof(why));
    CHECK(strstr(why, "API 3.0") && strstr(why, LICHEN_API_VERSION_TEXT));
    sd_app_scan_reason(SD_APP_SCAN_BAD_MANIFEST, LICHEN_MANIFEST_ERR_KEY, NULL, "x1", why, sizeof(why));
    CHECK(strstr(why, "x1") && strstr(why, "manifest"));
    char tiny[8];
    sd_app_scan_reason(SD_APP_SCAN_API, LICHEN_MANIFEST_OK, &m, "hello", tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny));
}

static void test_open(void) {
    sd_app_t a = {0};
    CHECK(sd_app_open(&a, "an update is held") == SD_OPEN_HELD && a.state == SD_APP_TILE); // the hold: nothing loads
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_LOAD && a.state == SD_APP_LOADING);
    CHECK(sd_app_open(&a, "held now") == SD_OPEN_WAIT); // a second open of a load that began is no new load
    CHECK(sd_app_load_done(&a, true) == SD_NEXT_OPEN && a.state == SD_APP_LOADED);
    CHECK(sd_app_open(&a, "held now") == SD_OPEN_NOW); // loaded: the hold applies to loads only
    a.removing = true;
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_REMOVED);
    CHECK(strcmp(sd_open_text(SD_OPEN_HELD), sd_open_text(SD_OPEN_BUSY)) != 0);
    CHECK(strstr(sd_open_text(SD_OPEN_HELD), "hold") != NULL);
}

static void test_failed_load(void) {
    sd_app_t a = {0};
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_LOAD);
    CHECK(sd_app_load_done(&a, false) == SD_NEXT_NONE && a.state == SD_APP_TILE);
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_LOAD); // the tile stays and can try again
    CHECK(sd_app_load_done(&a, true) == SD_NEXT_OPEN);
    CHECK(sd_app_load_done(&a, true) == SD_NEXT_NONE); // a stray second answer changes nothing
}

static void test_close_flow(void) {
    sd_app_t a = {0};
    sd_app_open(&a, NULL); // screen 1
    sd_app_load_done(&a, true);
    CHECK(sd_app_hooks_live(&a));
    CHECK(!sd_app_unload_begin(&a) && a.state == SD_APP_LOADED); // a screen exists: nothing may free the image
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_NOW && a.screens == 2); // a second screen
    CHECK(!sd_app_screen_deleted(&a) && a.screens == 1);       // one of two deleted: the app stays
    CHECK(!sd_app_unload_begin(&a));
    CHECK(sd_app_screen_deleted(&a) && a.screens == 0);        // the last one: the loader plans an unload
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_NOW);               // ... but it was opened again meanwhile
    CHECK(!sd_app_unload_begin(&a) && a.state == SD_APP_LOADED);
    CHECK(sd_app_screen_deleted(&a) && sd_app_unload_begin(&a) && a.state == SD_APP_UNLOADING);
    CHECK(!sd_app_hooks_live(&a)); // no hook is called from the first step on
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_BUSY);
    sd_app_unload_done(&a, true);
    CHECK(a.state == SD_APP_TILE);
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_LOAD); // load on open, again
}

static void test_no_free_before_deleted(void) {
    // the image may go only after LVGL deleted every screen of the app (#523 review H1): no timer can stand in for the event
    sd_app_t a = {0};
    sd_app_open(&a, NULL);
    sd_app_load_done(&a, true);
    a.closing = true; // even a stray close mark does not start an unload while a screen is there
    CHECK(!sd_app_unload_begin(&a));
    CHECK(sd_app_screen_deleted(&a));
    CHECK(sd_app_unload_begin(&a));
}

static void test_close_during_load(void) {
    sd_app_t a = {0};
    sd_app_open(&a, NULL);
    CHECK(!sd_app_screen_deleted(&a) && a.closing); // remembered, nothing to unload yet
    CHECK(sd_app_load_done(&a, true) == SD_NEXT_UNLOAD);
    CHECK(sd_app_unload_begin(&a));
    // reopened while loading: it stays
    sd_app_t b = {0};
    sd_app_open(&b, NULL);
    sd_app_screen_deleted(&b);
    CHECK(sd_app_open(&b, NULL) == SD_OPEN_WAIT && !b.closing);
    CHECK(sd_app_load_done(&b, true) == SD_NEXT_OPEN);
}

static void test_removal(void) {
    sd_app_t t = {0};
    CHECK(!sd_app_remove(&t) && t.removing);                      // a tile: nothing to unload
    CHECK(sd_app_open(&t, NULL) == SD_OPEN_REMOVED);
    sd_app_t l = {0};
    sd_app_open(&l, NULL);
    CHECK(sd_app_remove(&l));                                     // during a load: the unload follows it
    CHECK(sd_app_load_done(&l, true) == SD_NEXT_UNLOAD);
    CHECK(sd_app_unload_begin(&l));                               // removing starts the unload with the screen still there
    sd_app_t o = {0};
    o.state = SD_APP_LOADED;
    CHECK(sd_app_remove(&o) && sd_app_unload_begin(&o));
    sd_app_unload_done(&o, false);
    CHECK(o.state == SD_APP_FAULTED && !sd_app_hooks_live(&o)); // the image stays
    CHECK(!sd_app_remove(&o));                                    // faulted: nothing to unload
    sd_app_t f = {0};
    f.state = SD_APP_FAULTED;
    CHECK(sd_app_open(&f, NULL) == SD_OPEN_FAULTED);
    sd_app_unload_done(&f, true); // only an UNLOADING app ends an unload
    CHECK(f.state == SD_APP_FAULTED);
    CHECK(!sd_app_screen_deleted(&f));
}

static void test_removal_waits_for_screens(void) {
    // the card went with the app open: unregister's done does not free anything while a screen still waits to be deleted
    sd_app_t a = {0};
    sd_app_open(&a, NULL);
    sd_app_load_done(&a, true);
    CHECK(sd_app_remove(&a) && sd_app_unload_begin(&a)); // the screen is still there
    sd_app_unload_done(&a, true);
    CHECK(!sd_app_may_free(&a));
    CHECK(!sd_app_screen_deleted(&a)); // the deleted event: UNLOADING plans nothing, but the count is 0 now
    CHECK(sd_app_may_free(&a));
    sd_app_t t = {0};
    CHECK(sd_app_may_free(&t)); // a tile that was never opened
}

static void test_load_abort(void) {
    // an image was loaded, then main() gave no hooks: the Loading screen (screens == 1) is still there
    sd_app_t a = {0};
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_LOAD && a.screens == 1);
    CHECK(sd_app_load_abort(&a) && a.state == SD_APP_UNLOADING);
    sd_app_unload_done(&a, true);
    CHECK(a.state == SD_APP_TILE); // the unload's end lands, the image can go
    CHECK(!sd_app_screen_deleted(&a) && a.screens == 0 && a.state == SD_APP_TILE); // the screen's delete finds a tile
    CHECK(!sd_app_load_abort(&a)); // only a load aborts
    sd_app_t f = {0};
    sd_app_open(&f, NULL);
    sd_app_load_abort(&f);
    sd_app_unload_done(&f, false);
    CHECK(f.state == SD_APP_FAULTED && sd_app_open(&f, NULL) == SD_OPEN_FAULTED);
}

static void test_may_unregister(void) {
    // never for an app whose unload faulted (its image, tasks or timers may live), nor with a task of it left (#523 review H2)
    sd_app_t a = {0};
    CHECK(sd_app_may_unregister(&a, 0));                 // a tile that never loaded
    CHECK(!sd_app_may_unregister(&a, 1));
    a.state = SD_APP_LOADED;
    CHECK(sd_app_remove(&a) && sd_app_unload_begin(&a));
    CHECK(sd_app_may_unregister(&a, 0));                 // a clean unload up to the free step
    CHECK(!sd_app_may_unregister(&a, 2));
    sd_app_unload_done(&a, false);
    CHECK(!sd_app_may_unregister(&a, 0));                // faulted
    sd_app_t l = {.state = SD_APP_LOADING};
    CHECK(!sd_app_may_unregister(&l, 0));
    l.state = SD_APP_LOADED;
    CHECK(!sd_app_may_unregister(&l, 0));
}

static void test_unload_order(void) {
    // the invariant: stop hook, tasks, timers and UI calls, then the image; each step only after the one before
    sd_unload_step_t s = SD_UNLOAD_STOP;
    sd_unload_step_t want[] = {SD_UNLOAD_TASKS, SD_UNLOAD_UI, SD_UNLOAD_FREE, SD_UNLOAD_DONE};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        s = sd_unload_next(s, true);
        CHECK(s == want[i]);
    }
    CHECK(sd_unload_next(SD_UNLOAD_DONE, true) == SD_UNLOAD_DONE);
    // a failed step never reaches FREE
    for (int cur = SD_UNLOAD_STOP; cur <= SD_UNLOAD_FREE; cur++) {
        CHECK(sd_unload_next((sd_unload_step_t)cur, false) == SD_UNLOAD_FAULT);
    }
    CHECK(sd_unload_next(SD_UNLOAD_FAULT, true) == SD_UNLOAD_FAULT);
}

int main(void) {
    test_scan();
    test_open();
    test_failed_load();
    test_close_flow();
    test_no_free_before_deleted();
    test_removal_waits_for_screens();
    test_load_abort();
    test_may_unregister();
    test_close_during_load();
    test_removal();
    test_unload_order();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("sd_app_rules_test ok\n");
    return 0;
}
