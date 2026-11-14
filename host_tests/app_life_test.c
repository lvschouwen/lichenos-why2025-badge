// Host test: an app's lifecycle (#231, app_life.c). Every transition the OS makes, the ones it refuses, stop(reason)
// with its deadline (in time, refused, late, a second caller waiting, a hook that hangs past it, the clock wrapping),
// safe mode's FAULTED (#232) and which screens make an app OPEN or SUSPENDED.
#include "app_life.h"

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

static app_life_t fresh(void) {
    app_life_t r;
    memset(&r, 0xa5, sizeof(r));
    app_life_init(&r);
    return r;
}

static void test_record(void) {
    CHECK(sizeof(app_life_t) < 16); // #231: under 16 B per app
    app_life_t zero;
    memset(&zero, 0, sizeof(zero));
    CHECK(zero.state == LIFE_REGISTERED && zero.rest == LIFE_REGISTERED); // the table's .bss is already registered
    app_life_t r = fresh();
    CHECK(r.state == LIFE_REGISTERED && r.rest == LIFE_REGISTERED && r.fault == LIFE_FAULT_NONE && r.stops == 0);
}

static void test_names(void) {
    CHECK(strcmp(app_life_state_name(LIFE_REGISTERED), "registered") == 0);
    CHECK(strcmp(app_life_state_name(LIFE_SUSPENDED), "suspended") == 0);
    CHECK(strcmp(app_life_state_name(LIFE_FAULTED), "faulted") == 0);
    CHECK(strcmp(app_life_state_name(LIFE_STATE_COUNT), "?") == 0);
    CHECK(strcmp(app_life_state_name((life_state_t)-1), "?") == 0);
    CHECK(strcmp(app_life_reason_name(LIFE_STOP_POWER_OFF), "power off") == 0);
    CHECK(strcmp(app_life_reason_name(LIFE_STOP_REASON_COUNT), "?") == 0);
    CHECK(strcmp(app_life_fault_name(LIFE_FAULT_STOP_LATE), "stop late") == 0);
    CHECK(strcmp(app_life_fault_name(LIFE_FAULT_COUNT), "?") == 0);
    CHECK(app_life_deadline_ms(LIFE_STOP_UPDATE) == LIFE_DEADLINE_UPDATE_MS);
    CHECK(app_life_deadline_ms(LIFE_STOP_RESTART) == LIFE_DEADLINE_RESTART_MS);
    CHECK(app_life_deadline_ms(LIFE_STOP_POWER_OFF) == LIFE_DEADLINE_RESTART_MS);
    CHECK(app_life_deadline_ms(LIFE_STOP_REASON_COUNT) == 0);
}

static void test_boot(void) {
    app_life_t r = fresh();
    CHECK(app_life_started(&r) && r.state == LIFE_BACKGROUND && r.rest == LIFE_BACKGROUND);
    CHECK(!app_life_started(&r));        // only once
    CHECK(!app_life_fault_crashes(&r));  // safe mode decides before the start, never after it
    CHECK(r.state == LIFE_BACKGROUND && r.fault == LIFE_FAULT_NONE);

    app_life_t f = fresh();
    CHECK(app_life_fault_crashes(&f) && f.state == LIFE_FAULTED && f.rest == LIFE_FAULTED);
    CHECK(f.fault == LIFE_FAULT_CRASHES);
    CHECK(!app_life_started(&f)); // a background switched off never starts
    CHECK(!app_life_fault_crashes(&f));
}

static void test_screens(void) {
    app_life_t r = fresh();
    CHECK(app_life_show(&r, true, true) && r.state == LIFE_OPEN);
    CHECK(!app_life_show(&r, true, true)); // no change
    CHECK(app_life_show(&r, true, false) && r.state == LIFE_SUSPENDED);
    CHECK(app_life_show(&r, true, true) && r.state == LIFE_OPEN); // resumed
    CHECK(app_life_show(&r, false, false) && r.state == LIFE_REGISTERED);
    CHECK(!app_life_show(&r, false, true)); // closed stays closed, whatever on_top says

    app_life_t b = fresh();
    app_life_started(&b);
    CHECK(app_life_show(&b, true, false) && b.state == LIFE_SUSPENDED); // opened below another app's screen
    CHECK(app_life_show(&b, false, false) && b.state == LIFE_BACKGROUND); // the background part runs on
    CHECK(!app_life_started(&b));

    // Safe mode's FAULTED app can still be opened; closing it goes back to FAULTED, not to a running background
    app_life_t f = fresh();
    app_life_fault_crashes(&f);
    CHECK(app_life_show(&f, true, true) && f.state == LIFE_OPEN);
    CHECK(app_life_show(&f, false, false) && f.state == LIFE_FAULTED && f.fault == LIFE_FAULT_CRASHES);
}

static void test_stop_in_time(void) {
    app_life_t r = fresh();
    app_life_started(&r);
    app_life_show(&r, true, true);
    CHECK(app_life_stop_begin(&r, LIFE_STOP_UPDATE, 1000) == LIFE_BEGIN_OK);
    CHECK(r.state == LIFE_STOPPING && r.reason == LIFE_STOP_UPDATE && r.stops == 1);
    CHECK(r.deadline_ms == 1000 + LIFE_DEADLINE_UPDATE_MS);
    CHECK(!app_life_show(&r, false, false) && r.state == LIFE_STOPPING); // the update closes the screens: no change
    CHECK(!app_life_started(&r));
    CHECK(!app_life_fault_crashes(&r));
    CHECK(app_life_stop_end(&r, true, 2500) == LIFE_STOPPED);
    CHECK(r.state == LIFE_STOPPED && r.took_ms == 1500 && r.fault == LIFE_FAULT_NONE);
    CHECK(app_life_stop_end(&r, true, 2600) == LIFE_STATE_COUNT); // no stop under way
    CHECK(r.state == LIFE_STOPPED);
    CHECK(!app_life_show(&r, true, true) && r.state == LIFE_STOPPED); // a stopped app is not opened again
    app_life_note_tasks(&r, 2);
    CHECK(r.tasks_left == 2);
    app_life_note_tasks(&r, 1000);
    CHECK(r.tasks_left == 255);

    // The restart's save after the update stops again: the hook runs again, the verdict is the new one's
    CHECK(app_life_stop_begin(&r, LIFE_STOP_RESTART, 9000) == LIFE_BEGIN_OK);
    CHECK(r.tasks_left == 0 && r.stops == 2 && r.reason == LIFE_STOP_RESTART);
    CHECK(app_life_stop_end(&r, true, 9000) == LIFE_STOPPED && r.took_ms == 0);
}

static void test_stop_refused_and_late(void) {
    app_life_t r = fresh();
    CHECK(app_life_stop_begin(&r, LIFE_STOP_RESTART, 0) == LIFE_BEGIN_OK);
    CHECK(app_life_stop_end(&r, false, 10) == LIFE_FAULTED && r.fault == LIFE_FAULT_STOP_REFUSED);
    CHECK(!app_life_show(&r, true, true) && r.state == LIFE_FAULTED);

    // Stopped, but after the deadline: reported as late; exactly at the deadline is late already
    app_life_t l = fresh();
    app_life_stop_begin(&l, LIFE_STOP_RESTART, 100);
    CHECK(app_life_stop_end(&l, true, 100 + LIFE_DEADLINE_RESTART_MS) == LIFE_FAULTED);
    CHECK(l.fault == LIFE_FAULT_STOP_LATE && l.took_ms == LIFE_DEADLINE_RESTART_MS);
    app_life_t j = fresh();
    app_life_stop_begin(&j, LIFE_STOP_RESTART, 100);
    CHECK(app_life_stop_end(&j, true, 99 + LIFE_DEADLINE_RESTART_MS) == LIFE_STOPPED);

    // A hook that ran for minutes: took_ms is capped, not wrapped
    app_life_t h = fresh();
    app_life_stop_begin(&h, LIFE_STOP_UPDATE, 0);
    CHECK(app_life_stop_end(&h, true, 200000) == LIFE_FAULTED && h.took_ms == UINT16_MAX);

    // A faulted (late) app may be stopped again, and a later stop in time clears the fault
    CHECK(app_life_stop_begin(&l, LIFE_STOP_POWER_OFF, 20000) == LIFE_BEGIN_OK);
    CHECK(app_life_stop_end(&l, true, 20001) == LIFE_STOPPED && l.fault == LIFE_FAULT_NONE);

    // Safe mode's FAULTED app is stopped like any other (its hook still saves)
    app_life_t f = fresh();
    app_life_fault_crashes(&f);
    CHECK(app_life_stop_begin(&f, LIFE_STOP_UPDATE, 0) == LIFE_BEGIN_OK);
    CHECK(app_life_stop_end(&f, true, 5) == LIFE_STOPPED);
}

// A second stop(reason) while the first caller's hook runs: refused as busy, then the waiter waits for that verdict up
// to its OWN reason's deadline counted from when it began to wait. A hook that hangs (its task waits on the LVGL or the
// mesh lock) leaves the record STOPPING: the waiter gives up at its bound, the record is never forced to STOPPED, and
// nothing is deleted
static void test_stop_concurrent_and_hang(void) {
    app_life_t r = fresh();
    CHECK(app_life_stop_begin(&r, LIFE_STOP_UPDATE, 500) == LIFE_BEGIN_OK);
    CHECK(app_life_stop_begin(&r, LIFE_STOP_RESTART, 600) == LIFE_BEGIN_BUSY);
    CHECK(r.reason == LIFE_STOP_UPDATE && r.stops == 1 && r.deadline_ms == 500 + LIFE_DEADLINE_UPDATE_MS);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_RESTART, 600, 600) == LIFE_WAIT_PENDING);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_RESTART, 600, 599 + LIFE_DEADLINE_RESTART_MS) == LIFE_WAIT_PENDING);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_RESTART, 600, 600 + LIFE_DEADLINE_RESTART_MS) == LIFE_WAIT_OVERDUE);
    CHECK(r.state == LIFE_STOPPING);
    // The review case: an update's hook already hangs past its own 20 s when a restart arrives. The restart still
    // gives it its 8 s from now (as when it ran the hook itself and blocked on the mesh locks), not none
    uint32_t late = 500 + LIFE_DEADLINE_UPDATE_MS + 5000;
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_RESTART, late, late) == LIFE_WAIT_PENDING);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_RESTART, late, late + LIFE_DEADLINE_RESTART_MS - 1) == LIFE_WAIT_PENDING);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_RESTART, late, late + LIFE_DEADLINE_RESTART_MS) == LIFE_WAIT_OVERDUE);
    // An update waiting behind a restart's hook waits its own 20 s
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_UPDATE, 0, LIFE_DEADLINE_UPDATE_MS - 1) == LIFE_WAIT_PENDING);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_UPDATE, 0, LIFE_DEADLINE_UPDATE_MS) == LIFE_WAIT_OVERDUE);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_REASON_COUNT, 0, 0) == LIFE_WAIT_OVERDUE); // no bound: not waited for
    // The hook comes back after all, late but stopped: the waiter learns it stopped (the review's MeshCore case: a
    // 9 s mesh_stop of a restart must not leave LoRa running for the update that waited for it)
    CHECK(app_life_stop_end(&r, true, 900000) == LIFE_FAULTED && r.fault == LIFE_FAULT_STOP_LATE);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_RESTART, 600, 900001) == LIFE_WAIT_STOPPED);

    app_life_t n = fresh();
    app_life_stop_begin(&n, LIFE_STOP_RESTART, 0);
    app_life_stop_end(&n, false, 10); // refused: still running
    CHECK(app_life_wait_verdict(&n, LIFE_STOP_UPDATE, 0, 20) == LIFE_WAIT_FAILED);

    app_life_t s = fresh();
    app_life_stop_begin(&s, LIFE_STOP_RESTART, 0);
    app_life_stop_end(&s, true, 10);
    CHECK(app_life_wait_verdict(&s, LIFE_STOP_UPDATE, 0, 20) == LIFE_WAIT_STOPPED);
    CHECK(app_life_wait_verdict(&(app_life_t){0}, LIFE_STOP_UPDATE, 0, 0) == LIFE_WAIT_FAILED); // no stop asked
    app_life_t f = fresh();
    app_life_fault_crashes(&f);
    CHECK(app_life_wait_verdict(&f, LIFE_STOP_UPDATE, 0, 0) == LIFE_WAIT_FAILED); // safe mode's fault is no stop

    CHECK(app_life_stop_begin(&s, LIFE_STOP_REASON_COUNT, 0) == LIFE_BEGIN_BAD);
    CHECK(s.state == LIFE_STOPPED && s.stops == 1);
}

// A background part that starts again after a stop (a future restart path; today a stop ends in a reboot) returns the
// record to BACKGROUND and the screens move it again. Safe mode's crash fault outlives a stop and blocks the restart.
static void test_restart_and_crash_fault(void) {
    app_life_t r = fresh();
    app_life_started(&r);
    app_life_stop_begin(&r, LIFE_STOP_UPDATE, 0);
    CHECK(!app_life_started(&r)); // not while stopping
    app_life_stop_end(&r, true, 10);
    CHECK(!app_life_show(&r, true, true) && r.state == LIFE_STOPPED);
    CHECK(app_life_started(&r) && r.state == LIFE_BACKGROUND && r.fault == LIFE_FAULT_NONE);
    CHECK(app_life_show(&r, true, true) && r.state == LIFE_OPEN);
    CHECK(app_life_show(&r, false, false) && r.state == LIFE_BACKGROUND);

    app_life_t l = fresh();
    app_life_stop_begin(&l, LIFE_STOP_RESTART, 0);
    app_life_stop_end(&l, true, LIFE_DEADLINE_RESTART_MS); // late
    CHECK(app_life_started(&l) && l.state == LIFE_BACKGROUND && l.fault == LIFE_FAULT_NONE);

    app_life_t f = fresh();
    app_life_fault_crashes(&f);
    app_life_stop_begin(&f, LIFE_STOP_UPDATE, 0);
    CHECK(app_life_stop_end(&f, true, 5) == LIFE_STOPPED && f.fault == LIFE_FAULT_CRASHES); // the verdict stays
    CHECK(!app_life_started(&f) && f.state == LIFE_STOPPED);
    app_life_stop_begin(&f, LIFE_STOP_UPDATE, 10);
    CHECK(app_life_stop_end(&f, false, 20) == LIFE_FAULTED && f.fault == LIFE_FAULT_STOP_REFUSED);
    CHECK(!app_life_started(&f));
    app_life_stop_begin(&f, LIFE_STOP_UPDATE, 30);
    CHECK(app_life_stop_end(&f, true, 40) == LIFE_STOPPED && f.fault == LIFE_FAULT_CRASHES);
}

// The tick count wraps after 49 days: the deadline still holds across it
static void test_wrap(void) {
    app_life_t r     = fresh();
    uint32_t   start = UINT32_MAX - 1000;
    CHECK(app_life_stop_begin(&r, LIFE_STOP_UPDATE, start) == LIFE_BEGIN_OK);
    CHECK(r.deadline_ms == LIFE_DEADLINE_UPDATE_MS - 1001);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_UPDATE, start, start + 500) == LIFE_WAIT_PENDING);
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_UPDATE, start, 100) == LIFE_WAIT_PENDING); // past the wrap
    CHECK(app_life_wait_verdict(&r, LIFE_STOP_UPDATE, start, LIFE_DEADLINE_UPDATE_MS) == LIFE_WAIT_OVERDUE);
    CHECK(app_life_stop_end(&r, true, 2000) == LIFE_STOPPED && r.took_ms == 3001);
}

#define LO       1
#define HI       32
#define SET_BASE 200 // SCREEN_APP_BASE: app i's Settings page
#define CHAT     120 // a fixed MeshCore screen id, attributed by the context it was opened in

static void test_owner(void) {
    CHECK(app_life_screen_owner(0, 0, LO, HI, SET_BASE) == 0);      // home
    CHECK(app_life_screen_owner(3, 0, LO, HI, SET_BASE) == 3);      // app index 2's own screen
    CHECK(app_life_screen_owner(LO, 9, LO, HI, SET_BASE) == 1);     // the id wins over the push context
    CHECK(app_life_screen_owner(HI, 0, LO, HI, SET_BASE) == 32);
    CHECK(app_life_screen_owner(SET_BASE, 0, LO, HI, SET_BASE) == 1); // app index 0's Settings page
    CHECK(app_life_screen_owner(SET_BASE + 31, 0, LO, HI, SET_BASE) == 32);
    CHECK(app_life_screen_owner(SET_BASE + 32, 0, LO, HI, SET_BASE) == 0); // past the range
    CHECK(app_life_screen_owner(CHAT, 1, LO, HI, SET_BASE) == 1);  // a chat opened by MeshCore (slot 1)
    CHECK(app_life_screen_owner(CHAT, 0, LO, HI, SET_BASE) == 0);  // an OS page
    CHECK(app_life_screen_owner(CHAT, -4, LO, HI, SET_BASE) == 0);
}

static void view(int const *owners, int depth, int slot, bool want_shown, bool want_top, int line) {
    bool shown = !want_shown, top = !want_top;
    app_life_view(owners, depth, slot, &shown, &top);
    if (shown != want_shown || top != want_top) {
        printf("FAIL %s:%d: view of %d: shown %d on_top %d\n", __FILE__, line, slot, shown, top);
        failures++;
    }
}

static void test_view(void) {
    int const home[] = {0};
    view(home, 1, 3, false, false, __LINE__);
    view(home, 0, 3, false, false, __LINE__); // an empty stack (booting)
    view(home, 1, 0, false, false, __LINE__); // slot 0 is the OS: never an app

    int const one[] = {0, 3};
    view(one, 2, 3, true, true, __LINE__);
    view(one, 2, 4, false, false, __LINE__);

    // OS pages above it: still on top. Only a chat of its own (opened from a notification, no app screen below it)
    int const own[] = {0, 3, 0, 0};
    view(own, 4, 3, true, true, __LINE__);
    int const chat_only[] = {0, 1};
    view(chat_only, 2, 1, true, true, __LINE__);

    // Another app's screen above it: suspended; the other is on top
    int const two[] = {0, 3, 0, 5, 0};
    view(two, 5, 3, true, false, __LINE__);
    view(two, 5, 5, true, true, __LINE__);
    // Its own screen again above the other's (MeshCore's chat over Weather over MeshCore): on top
    int const back[] = {0, 1, 5, 1};
    view(back, 4, 1, true, true, __LINE__);
    view(back, 4, 5, true, false, __LINE__);
}

int main(void) {
    test_record();
    test_names();
    test_boot();
    test_screens();
    test_stop_in_time();
    test_stop_refused_and_late();
    test_stop_concurrent_and_hang();
    test_restart_and_crash_fault();
    test_wrap();
    test_owner();
    test_view();
    if (failures) {
        printf("app_life_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("app_life_test: all passed\n");
    return 0;
}
