// Host test (#400): starting MeshCore is hand-only on every path. The paths as the firmware takes them: a shape-key
// shortcut runs under raw_keys.c's mark of the event's origin, an LVGL key (home tile, grid, notification, Settings
// row) is unmarked and the bound last-key origin decides, "fw ui open" runs under the console mark. The start rule
// is meshcore_run_request's: mesh_run_start_hand_only(state, key_cause_hand_only_ok(key_cause_now())).
#include "key_cause.h"
#include "mesh_run.h"

#include <stdio.h>
#include <string.h>

static int  failures;
static bool last_injected;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool fake_last(void) {
    return last_injected;
}

typedef enum { PATH_SHAPE_KEY, PATH_LVGL_KEY, PATH_CONSOLE } path_t;

// The start rule as meshcore_run_request applies it, on one path for a key of this origin: true when refused
static bool refused(path_t path, bool injected, mesh_run_t r) {
    key_cause_t previous = KEY_CAUSE_NONE;
    last_injected        = false; // the LVGL key before: a hand, so a shape key's mark must be what decides
    if (path == PATH_SHAPE_KEY) {
        previous = key_cause_enter(injected ? KEY_CAUSE_INJECTED : KEY_CAUSE_HAND);
    } else if (path == PATH_LVGL_KEY) {
        last_injected = injected;
    } else {
        last_injected = true; // the bench's last key was injected; the command itself is the operator's
        previous      = key_cause_enter(KEY_CAUSE_CONSOLE);
    }
    bool no = mesh_run_start_hand_only(r, key_cause_hand_only_ok(key_cause_now()));
    if (path != PATH_LVGL_KEY) {
        key_cause_leave(previous);
    }
    CHECK(key_cause_enter(KEY_CAUSE_NONE) == KEY_CAUSE_NONE); // every path left no mark behind
    return no;
}

static void test_matrix(void) {
    mesh_run_t stopped = MESH_RUN_INIT;
    mesh_run_t user    = {MESH_RUN_STOPPED, MESH_WHY_USER, false};
    mesh_run_t update  = {MESH_RUN_STOPPED, MESH_WHY_UPDATE, false};
    mesh_run_t running = {MESH_RUN_RUNNING, MESH_WHY_USER, false};
    mesh_run_t start   = {MESH_RUN_STARTING, MESH_WHY_USER, false};
    mesh_run_t paused  = {MESH_RUN_STOPPED, MESH_WHY_USER, true};
    for (int p = PATH_SHAPE_KEY; p <= PATH_CONSOLE; p++) {
        for (int inj = 0; inj <= 1; inj++) {
            bool refuse = inj && p != PATH_CONSOLE; // the console has no key origin: the command is the operator's
            CHECK(refused((path_t)p, inj, stopped) == refuse);
            CHECK(refused((path_t)p, inj, user) == refuse);
            CHECK(refused((path_t)p, inj, paused) == refuse); // refused anyway (the pause), the rule fails safe
            // Nothing would start: navigating to a running MeshCore stays free for an injected key
            CHECK(!refused((path_t)p, inj, running));
            CHECK(!refused((path_t)p, inj, start));
            CHECK(!refused((path_t)p, inj, update));
        }
    }
}

static void test_cause(void) {
    key_cause_bind_last_key(NULL);
    CHECK(key_cause_now() == KEY_CAUSE_INJECTED); // nothing bound: no key is known to be a hand
    key_cause_bind_last_key(fake_last);
    last_injected = false;
    CHECK(key_cause_now() == KEY_CAUSE_HAND);
    last_injected = true;
    CHECK(key_cause_now() == KEY_CAUSE_INJECTED);
    // Marks nest and come back as they were
    key_cause_t outer = key_cause_enter(KEY_CAUSE_HAND);
    CHECK(outer == KEY_CAUSE_NONE);
    CHECK(key_cause_now() == KEY_CAUSE_HAND); // the mark wins over the last LVGL key
    key_cause_t inner = key_cause_enter(KEY_CAUSE_INJECTED);
    CHECK(inner == KEY_CAUSE_HAND && key_cause_now() == KEY_CAUSE_INJECTED);
    key_cause_leave(inner);
    CHECK(key_cause_now() == KEY_CAUSE_HAND);
    key_cause_leave(outer);
    CHECK(key_cause_now() == KEY_CAUSE_INJECTED);
    CHECK(key_cause_hand_only_ok(KEY_CAUSE_HAND) && key_cause_hand_only_ok(KEY_CAUSE_CONSOLE));
    CHECK(!key_cause_hand_only_ok(KEY_CAUSE_INJECTED) && !key_cause_hand_only_ok(KEY_CAUSE_NONE));
    CHECK(!key_cause_hand_only_ok((key_cause_t)42)); // an unknown cause is no hand
    CHECK(strcmp(key_cause_name(KEY_CAUSE_INJECTED), "injected key") == 0);
    CHECK(strcmp(key_cause_name((key_cause_t)42), "none") == 0);
}

int main(void) {
    test_cause();
    test_matrix();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("key_cause_test: ok\n");
    return 0;
}
