// Host test (#400): starting MeshCore is hand-only on every path. The paths run the firmware's own pieces: the cause
// (key_cause.c, with the getter built as shortcuts.c builds it on key_cause_last_key_injected), the UI service's queue
// that carries the cause to a deferred call (ui_call_queue.c), and the start rule meshcore_run_request applies
// (mesh_run_start_refusal). A shape-key shortcut runs under raw_keys.c's mark of its event's origin, a tile's Enter
// is judged while LVGL reads the key (or when the call it queued runs), "fw ui open" and the spectrum resume are
// KEY_CAUSE_SYSTEM.
#include "key_cause.h"
#include "mesh_run.h"
#include "ui_call_queue.h"

#include <stdio.h>
#include <string.h>

static int  failures;
static bool indev_active;         // lv_indev_active() != NULL: LVGL is reading the keyboard
static bool last_injected = true; // keyboard.c's: true before the first key

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool getter(void) { // shortcuts.c's last_key_injected
    return key_cause_last_key_injected(indev_active, last_injected);
}

static mesh_run_t const STOPPED = MESH_RUN_INIT;

// The start as meshcore_run_request asks for it now: true when it may go ahead
static bool starts(mesh_run_t r) {
    return mesh_run_start_refusal(r, true, key_cause_now()) == NULL;
}

// LVGL reads one key: a handler runs while the device is active
static void lvgl_key(bool injected) {
    last_injected = injected;
    indev_active  = true;
}

static void lvgl_read_done(void) {
    indev_active = false;
}

static void test_sync_paths(void) {
    mesh_run_t user    = {MESH_RUN_STOPPED, MESH_WHY_USER, false};
    mesh_run_t running = {MESH_RUN_RUNNING, MESH_WHY_USER, false};
    mesh_run_t update  = {MESH_RUN_STOPPED, MESH_WHY_UPDATE, false};
    for (int inj = 0; inj <= 1; inj++) {
        // A shape-key shortcut: raw_keys.c marks its listeners; the last LVGL key (a hand's) does not count
        lvgl_key(false);
        lvgl_read_done();
        key_cause_t previous = key_cause_enter(inj ? KEY_CAUSE_INJECTED : KEY_CAUSE_HAND);
        CHECK(starts(STOPPED) == !inj);
        CHECK(starts(user) == !inj);
        key_cause_leave(previous);
        // A tile's or a row's Enter handled while LVGL reads it
        lvgl_key(inj);
        CHECK(starts(STOPPED) == !inj);
        lvgl_read_done();
        // "fw ui open": the operator's command, whatever key came last
        lvgl_key(true);
        lvgl_read_done();
        previous = key_cause_enter(KEY_CAUSE_SYSTEM);
        CHECK(starts(STOPPED));
        key_cause_leave(previous);
    }
    // Nothing would start: navigating to a running MeshCore stays free; the radio's own reasons come first
    lvgl_key(true);
    CHECK(strcmp(mesh_run_start_refusal(running, true, key_cause_now()), "already running") == 0);
    CHECK(strcmp(mesh_run_start_refusal(update, true, key_cause_now()), "stopped for update") == 0);
    CHECK(strcmp(mesh_run_start_refusal(STOPPED, false, key_cause_now()), "no LoRa radio") == 0);
    CHECK(strcmp(mesh_run_start_refusal(STOPPED, true, key_cause_now()), MESH_RUN_HAND_ONLY_REFUSAL) == 0);
    lvgl_read_done();
}

// ---- deferred calls (home.c's open_selected, the grid's open_tile) ----

static bool       ran, started;
static mesh_run_t job_state;

static void deferred_start(void *arg) {
    (void)arg;
    ran     = true;
    started = starts(job_state);
}

static bool schedule_ok(ui_call_fn_t fn, void *arg) {
    (void)fn;
    (void)arg;
    return true; // the test drains by hand
}

static ui_call_queue_t queue;

// Queues deferred_start while LVGL handles a key of this origin, then a key of the other origin is read, then the
// queue drains on a later pass with no device being read: whether MeshCore started
static bool queued_then_run(bool queued_injected, bool then_injected) {
    memset(&queue, 0, sizeof(queue));
    ran = started = false;
    job_state     = STOPPED;
    lvgl_key(queued_injected);
    CHECK(ui_call_queue_hand_over(&queue, deferred_start, NULL, 0, schedule_ok, schedule_ok));
    lvgl_read_done();
    lvgl_key(then_injected); // the same poll's next key, read before the drain
    lvgl_read_done();
    ui_call_queue_drain(&queue);
    CHECK(ran);
    CHECK(key_cause_enter(KEY_CAUSE_NONE) == KEY_CAUSE_NONE); // the drain left no mark behind
    return started;
}

static void test_deferred(void) {
    CHECK(!queued_then_run(true, false)); // an injected Enter, a hand's key read after it: still refused
    CHECK(queued_then_run(false, true));  // a hand's Enter, an injected key after it: still allowed
    CHECK(!queued_then_run(true, true));
    CHECK(queued_then_run(false, false));
    // Queued by another task (no device read, no mark): fails closed even after a hand's key
    memset(&queue, 0, sizeof(queue));
    ran = started = false;
    lvgl_key(false);
    lvgl_read_done();
    CHECK(ui_call_queue_hand_over(&queue, deferred_start, NULL, 0, schedule_ok, schedule_ok));
    lvgl_key(false);
    ui_call_queue_drain(&queue); // even if a stale device were active then: the recorded mark decides
    lvgl_read_done();
    CHECK(ran && !started);
    // Queued under "fw ui open": the operator's
    memset(&queue, 0, sizeof(queue));
    ran                  = started = false;
    key_cause_t previous = key_cause_enter(KEY_CAUSE_SYSTEM);
    CHECK(ui_call_queue_hand_over(&queue, deferred_start, NULL, 0, schedule_ok, schedule_ok));
    key_cause_leave(previous);
    ui_call_queue_drain(&queue);
    CHECK(ran && started);
}

static void test_unmarked(void) {
    // A timer or an async call (the full queue's fallback) with no device read: no key set it off
    lvgl_key(false);
    lvgl_read_done();
    CHECK(key_cause_now() == KEY_CAUSE_INJECTED);
    CHECK(!starts(STOPPED));
    // Before the first key: keyboard.c's last_injected starts true
    last_injected = true;
    indev_active  = true;
    CHECK(!starts(STOPPED));
    indev_active = false;
    // The spectrum resume (main.c): meshcore_run_request_system's cause
    CHECK(mesh_run_start_refusal(STOPPED, true, KEY_CAUSE_SYSTEM) == NULL);
}

static void test_cause(void) {
    key_cause_bind_last_key(NULL);
    CHECK(key_cause_now() == KEY_CAUSE_INJECTED); // nothing bound: no key is known to be a hand
    key_cause_bind_last_key(getter);
    lvgl_key(false);
    CHECK(key_cause_now() == KEY_CAUSE_HAND);
    // Marks nest and come back as they were; a mark wins over the key being read
    key_cause_t outer = key_cause_enter(KEY_CAUSE_INJECTED);
    CHECK(outer == KEY_CAUSE_NONE && key_cause_now() == KEY_CAUSE_INJECTED);
    key_cause_t inner = key_cause_enter(KEY_CAUSE_HAND);
    CHECK(inner == KEY_CAUSE_INJECTED && key_cause_now() == KEY_CAUSE_HAND);
    key_cause_leave(inner);
    CHECK(key_cause_now() == KEY_CAUSE_INJECTED);
    key_cause_leave(outer);
    CHECK(key_cause_now() == KEY_CAUSE_HAND);
    lvgl_read_done();
    CHECK(!key_cause_last_key_injected(true, false));
    CHECK(key_cause_last_key_injected(false, false) && key_cause_last_key_injected(true, true));
    CHECK(key_cause_hand_only_ok(KEY_CAUSE_HAND) && key_cause_hand_only_ok(KEY_CAUSE_SYSTEM));
    CHECK(!key_cause_hand_only_ok(KEY_CAUSE_INJECTED) && !key_cause_hand_only_ok(KEY_CAUSE_NONE));
    CHECK(!key_cause_hand_only_ok((key_cause_t)42)); // an unknown cause is no hand
    CHECK(strcmp(key_cause_name(KEY_CAUSE_INJECTED), "injected key") == 0);
    CHECK(strcmp(key_cause_name((key_cause_t)42), "none") == 0);
}

int main(void) {
    test_cause();
    test_sync_paths();
    test_deferred();
    test_unmarked();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("key_cause_test: ok\n");
    return 0;
}
