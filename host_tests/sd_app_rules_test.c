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

static void test_budget(void) {
    CHECK(sd_app_task_budget(0) == 24u * 1024u); // not declared: main 4 + stop 4 + one 16 KiB task
    CHECK(sd_app_task_budget(24) == 24u * 1024u);
    CHECK(sd_app_task_budget(8) == 8u * 1024u); // declared: as is, even below the default
    CHECK(sd_app_task_budget(48) == 48u * 1024u);
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

// exit() (#533): the app's own task asks for the close; the normal unload does the rest
static void test_exit(void) {
    sd_app_t a = {.state = SD_APP_LOADED, .screens = 2};
    CHECK(sd_app_exit(&a) == SD_EXIT_HOME && !a.closing); // the shell closes the screens; the last delete event unloads
    sd_app_t n = {.state = SD_APP_LOADED, .screens = 0};
    CHECK(sd_app_exit(&n) == SD_EXIT_UNLOAD && n.closing && sd_app_unload_begin(&n));
    sd_app_t l = {.state = SD_APP_LOADING, .screens = 1};
    CHECK(sd_app_exit(&l) == (SD_EXIT_WAKE | SD_EXIT_HOME) && l.closing); // before its hooks: the loader wakes, the placeholder goes
    CHECK(sd_app_load_done(&l, true) == SD_NEXT_UNLOAD); // hooks were set in time: unloads, never opens
    sd_app_t l0 = {.state = SD_APP_LOADING, .screens = 0};
    CHECK(sd_app_exit(&l0) == SD_EXIT_WAKE);
    sd_app_t t = {.state = SD_APP_TILE}, u = {.state = SD_APP_UNLOADING}, f = {.state = SD_APP_FAULTED};
    CHECK(sd_app_exit(&t) == SD_EXIT_NONE && sd_app_exit(&u) == SD_EXIT_NONE && sd_app_exit(&f) == SD_EXIT_NONE);
    CHECK(!t.closing && u.state == SD_APP_UNLOADING);
    // #532: the image is freed after lichen_unregister_app; entering the stale context was refused and logged there
    CHECK(sd_app_free_enters(false) && !sd_app_free_enters(true));
}

// #546: a close before the read starts drops the load; a removal or an open screen does not
static void test_load_skip(void) {
    sd_app_t a = {.state = SD_APP_LOADING, .screens = 0, .closing = true};
    CHECK(sd_app_load_skip(&a) && a.state == SD_APP_TILE && !a.closing);
    sd_app_t open = {.state = SD_APP_LOADING, .screens = 1};
    CHECK(!sd_app_load_skip(&open) && open.state == SD_APP_LOADING);
    sd_app_t shown = {.state = SD_APP_LOADING, .screens = 1, .closing = true}; // closing alone is not enough
    CHECK(!sd_app_load_skip(&shown) && shown.state == SD_APP_LOADING);
    sd_app_t rm = {.state = SD_APP_LOADING, .closing = true, .removing = true};
    CHECK(!sd_app_load_skip(&rm) && rm.state == SD_APP_LOADING);
    sd_app_t l = {.state = SD_APP_LOADED, .closing = true};
    CHECK(!sd_app_load_skip(&l));
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_LOAD); // a reopen after the skip loads again
}

// #548: the bar's percent and its throttle (about 10 updates a second; never 100 from the reader's bytes)
static void test_progress(void) {
    CHECK(sd_progress_pct(0, 0) == 0 && sd_progress_pct(0, 1000) == 0 && sd_progress_pct(500, 1000) == 50);
    CHECK(sd_progress_pct(999, 1000) == 99 && sd_progress_pct(1000, 1000) == 100 && sd_progress_pct(2000, 1000) == 100);
    CHECK(sd_progress_pct((size_t)1 << 31, (size_t)1 << 32) == 50); // no overflow in the multiply
    sd_progress_t p = {0};
    int           v = -1;
    CHECK(sd_progress_due(&p, 0, 1000, 5000, &v) && v == 0); // the first update is always handed out
    sd_progress_sent(&p, v, 5000);
    CHECK(!sd_progress_due(&p, 100, 1000, 5050, &v));        // 50 ms: too soon
    CHECK(sd_progress_due(&p, 100, 1000, 5100, &v) && v == 10);
    CHECK(sd_progress_due(&p, 100, 1000, 5200, &v) && v == 10); // not sent (the lock was busy): offered again
    sd_progress_sent(&p, v, 5200);
    CHECK(!sd_progress_due(&p, 105, 1000, 5400, &v));        // same percent: nothing to draw
    CHECK(!sd_progress_due(&p, 50, 1000, 5500, &v));         // never backwards
    CHECK(sd_progress_due(&p, 5000, 1000, 5600, &v) && v == 99); // bytes past the size never show a full bar
    // the clock wrapped: 0x80 is 144 ms after 0xFFFFFFF0
    sd_progress_t w = {.any = true, .last_pct = 10, .last_ms = 0xFFFFFFF0u};
    CHECK(!sd_progress_due(&w, 200, 1000, 0x10u, &v));       // 32 ms later, across the wrap
    CHECK(sd_progress_due(&w, 200, 1000, 0x80u, &v) && v == 20);
}

// The real read order of esp_elf_relocate_source: the ELF header, the section table at the file's end, then the segments
// ascending. Counting bytes taken (not the furthest offset) the bar climbs instead of jumping to the end at once.
static void test_progress_read_order(void) {
    size_t const total = 100000, reads[] = {64, 40 * 12, 20000, 30000, 25000, 20000}; // sums to 95 kB: the rest is unread
    sd_progress_t p = {0};
    size_t        done = 0;
    uint32_t      now  = 1000;
    int           v, shown = 0, last = -1;
    for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
        done += reads[i];
        now += 150;
        if (sd_progress_due(&p, done, total, now, &v)) {
            CHECK(v > last && v < 100);
            last = v;
            shown++;
            sd_progress_sent(&p, v, now);
        }
    }
    CHECK(shown == 5 && last == 95); // the tiny header reads show 0 once; none at 100
}

static sd_app_t unloading_app(void) {
    sd_app_t a = {.state = SD_APP_LOADED, .closing = true};
    CHECK(sd_app_unload_begin(&a));
    return a;
}

// #541: a stop hook or task wait that overran is resumed when the last task ends; later faults never are
static void test_late_unload(void) {
    sd_app_t a = unloading_app();
    sd_app_unload_fault(&a, SD_UNLOAD_TASKS);
    CHECK(a.state == SD_APP_FAULTED && a.late);
    CHECK(!sd_app_late_begin(&a, 1)); // a task of the app still runs: nothing is freed
    CHECK(a.state == SD_APP_FAULTED && a.late);
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_BUSY); // a slow close, not a dead app: try again
    CHECK(sd_app_late_begin(&a, 0) && a.state == SD_APP_UNLOADING);
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_BUSY); // a reopen meanwhile waits
    CHECK(!sd_app_late_begin(&a, 0)); // once
    sd_app_unload_done(&a, true);
    CHECK(a.state == SD_APP_TILE && !a.late);

    a = unloading_app(); // the stop hook failed
    sd_app_unload_fault(&a, SD_UNLOAD_STOP);
    CHECK(sd_app_late_begin(&a, 0));

    a = unloading_app(); // timers/UI failed: callbacks into the image may be queued
    sd_app_unload_fault(&a, SD_UNLOAD_UI);
    CHECK(a.state == SD_APP_FAULTED && !a.late && !sd_app_late_begin(&a, 0));

    a = unloading_app(); // holdings could not be released (unload_done false)
    sd_app_unload_done(&a, false);
    CHECK(a.state == SD_APP_FAULTED && !sd_app_late_begin(&a, 0));

    a = unloading_app(); // a screen of the app still exists (removal): the free waits for its delete event
    a.screens = 1;
    sd_app_unload_fault(&a, SD_UNLOAD_TASKS);
    CHECK(!sd_app_late_begin(&a, 0));

    a = unloading_app(); // a hung task: after the loader's patience the open answers as for a dead app, the resume stays possible
    sd_app_unload_fault(&a, SD_UNLOAD_TASKS);
    sd_app_late_expire(&a);
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_FAULTED);
    CHECK(sd_app_late_begin(&a, 0) && !a.late_stale);
    sd_app_t g = {.state = SD_APP_LOADED};
    sd_app_late_expire(&g);
    CHECK(!g.late_stale); // only a resumable fault expires

    // #541 review: one message between the three askers, and a screen that left later asks again
    a = unloading_app();
    a.screens = 1; // a removal's screen still animating out
    sd_app_unload_fault(&a, SD_UNLOAD_TASKS);
    CHECK(!sd_app_late_claim(&a));
    CHECK(!sd_app_screen_deleted(&a) && a.screens == 0); // not an unload to plan: it is faulted
    CHECK(sd_app_late_claim(&a) && !sd_app_late_claim(&a)); // once
    sd_app_late_unclaim(&a); // the post failed: asked again later
    CHECK(sd_app_late_claim(&a));
    CHECK(!sd_app_late_begin(&a, 1) && !a.late_queued && a.late); // consumed with a task back: a new ask follows its end
    CHECK(sd_app_late_claim(&a) && sd_app_late_begin(&a, 0));
    CHECK(!sd_app_late_claim(&a));
    sd_app_unload_done(&a, true);
    CHECK(!sd_app_late_claim(&a));

    a = unloading_app(); // a later fault clears a stale claim
    sd_app_unload_fault(&a, SD_UNLOAD_TASKS);
    CHECK(sd_app_late_claim(&a) && !sd_app_late_begin(&a, 1));
    CHECK(sd_app_open(&a, NULL) == SD_OPEN_BUSY);

    sd_app_t t = {.state = SD_APP_TILE, .late = true}; // never from another state
    CHECK(!sd_app_late_begin(&t, 0));
}

// #553: which landed file is an app's manifest, and which apps read it
static void test_push_rules(void) {
    char id[LICHEN_MANIFEST_ID_MAX];
    bool man = false;
    char const *dir = "/sd/lichen/apps";
    CHECK(sd_app_push_target(dir, "/sd/lichen/apps/doom/manifest", id, sizeof(id), &man) && !strcmp(id, "doom") && man);
    CHECK(sd_app_push_target(dir, "/sd/lichen/apps/doom/doom.elf", id, sizeof(id), &man) && !strcmp(id, "doom") && !man);
    CHECK(!sd_app_push_target(dir, "/sd/lichen/apps/doom/save/manifest", id, sizeof(id), &man)); // the app's data
    CHECK(!sd_app_push_target(dir, "/sd/lichen/apps/doom", id, sizeof(id), &man));
    CHECK(!sd_app_push_target(dir, "/sd/lichen/apps//manifest", id, sizeof(id), &man));
    CHECK(!sd_app_push_target(dir, "/sd/lichen/apps/.hid/manifest", id, sizeof(id), &man));
    CHECK(!sd_app_push_target(dir, "/sd/lichen/apps/averyveryverylongid/manifest", id, sizeof(id), &man));
    CHECK(!sd_app_push_target(dir, "/sd/lichen/appsx/doom/manifest", id, sizeof(id), &man));
    CHECK(!sd_app_push_target(dir, "/sd/lichen/messages/doom/manifest", id, sizeof(id), &man));
    CHECK(!sd_app_push_target(dir, "/storage/doom/manifest", id, sizeof(id), &man));
    CHECK(!sd_app_push_target(dir, "/sd/lichen/apps/doom/manifest", id, 4, &man)); // does not fit the caller's buffer

    {
        sd_app_t l = {.state = SD_APP_LOADING, .screens = 1};
        CHECK(sd_app_screen_loading(&l, false));
        CHECK(!sd_app_screen_loading(&l, true));
        l.screens = 0;
        CHECK(!sd_app_screen_loading(&l, false));
        l.screens = 1;
        l.state   = SD_APP_FAULTED;
        CHECK(!sd_app_screen_loading(&l, false));
        l.state = SD_APP_UNLOADING;
        CHECK(!sd_app_screen_loading(&l, false));
    }
    CHECK(sd_app_rescan_wanted(true, SD_APP_TILE, false, false));
    CHECK(sd_app_rescan_wanted(true, SD_APP_LOADED, false, false)); // read at its unload
    CHECK(sd_app_rescan_wanted(true, SD_APP_LOADING, false, false));
    CHECK(sd_app_rescan_wanted(true, SD_APP_UNLOADING, false, false));
    CHECK(!sd_app_rescan_wanted(false, SD_APP_TILE, false, false)); // the ELF is read on the next open
    CHECK(!sd_app_rescan_wanted(true, SD_APP_FAULTED, false, false));
    CHECK(!sd_app_rescan_wanted(true, SD_APP_TILE, true, false));
    CHECK(!sd_app_rescan_wanted(true, SD_APP_TILE, false, true));
}

int main(void) {
    test_push_rules();
    test_budget();
    test_late_unload();
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
    test_exit();
    test_load_skip();
    test_progress();
    test_progress_read_order();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("sd_app_rules_test ok\n");
    return 0;
}
