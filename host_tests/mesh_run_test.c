// Host test: MeshCore start at boot, start and stop on demand (#103): the state machine and its texts
#include "mesh_run.h"

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

static bool is(mesh_run_t r, mesh_run_state_t s, mesh_run_why_t why) {
    return r.state == s && (s != MESH_RUN_STOPPED || r.why == why);
}

static void machine(void) {
    // Autostart off: stopped, not wanting the radio; one start at a time
    mesh_run_t r = MESH_RUN_INIT;
    CHECK(is(r, MESH_RUN_STOPPED, MESH_WHY_AUTOSTART_OFF));
    CHECK(!mesh_run_wants_radio(r));
    CHECK(!mesh_run_event(&r, MESH_EV_STOP));      // nothing to stop
    CHECK(!mesh_run_event(&r, MESH_EV_DONE_OK));   // nothing under way
    CHECK(mesh_run_event(&r, MESH_EV_START));
    CHECK(r.state == MESH_RUN_STARTING && mesh_run_busy(r) && mesh_run_wants_radio(r));
    CHECK(!mesh_run_event(&r, MESH_EV_START));     // a second request while starting: refused (no double start)
    CHECK(!mesh_run_event(&r, MESH_EV_STOP));      // and no stop in the middle of a start
    CHECK(!mesh_run_event(&r, MESH_EV_SEEN_UP));   // a sync does not overtake the start's own result
    CHECK(mesh_run_event(&r, MESH_EV_DONE_OK));
    CHECK(r.state == MESH_RUN_RUNNING && !mesh_run_busy(r));
    CHECK(!mesh_run_event(&r, MESH_EV_START));     // already running

    // Stop on request, then start again
    CHECK(mesh_run_event(&r, MESH_EV_STOP));
    CHECK(r.state == MESH_RUN_STOPPING && mesh_run_busy(r));
    CHECK(!mesh_run_event(&r, MESH_EV_STOP));
    CHECK(!mesh_run_event(&r, MESH_EV_START));
    CHECK(mesh_run_event(&r, MESH_EV_DONE_OK));
    CHECK(is(r, MESH_RUN_STOPPED, MESH_WHY_USER) && !mesh_run_wants_radio(r));
    CHECK(mesh_run_event(&r, MESH_EV_START));
    CHECK(mesh_run_event(&r, MESH_EV_DONE_OK));
    CHECK(r.state == MESH_RUN_RUNNING);

    // A stop that failed leaves it running; a start that failed leaves it stopped, wanting the radio (red if down)
    CHECK(mesh_run_event(&r, MESH_EV_STOP));
    CHECK(mesh_run_event(&r, MESH_EV_DONE_FAIL));
    CHECK(r.state == MESH_RUN_RUNNING);
    mesh_run_t f = MESH_RUN_INIT;
    CHECK(mesh_run_event(&f, MESH_EV_START));
    CHECK(mesh_run_event(&f, MESH_EV_DONE_FAIL));
    CHECK(is(f, MESH_RUN_STOPPED, MESH_WHY_FAILED) && mesh_run_wants_radio(f));
    CHECK(mesh_run_event(&f, MESH_EV_START)); // a retry is allowed

    // Syncs: a stop that finished late, a task found running
    CHECK(mesh_run_event(&r, MESH_EV_SEEN_DOWN));
    CHECK(is(r, MESH_RUN_STOPPED, MESH_WHY_FAILED));
    CHECK(!mesh_run_event(&r, MESH_EV_SEEN_DOWN));
    CHECK(mesh_run_event(&r, MESH_EV_SEEN_UP));
    CHECK(r.state == MESH_RUN_RUNNING);
    CHECK(!mesh_run_event(&r, MESH_EV_SEEN_UP));

    // An update is final, from any state
    mesh_run_state_t const from[] = {MESH_RUN_STOPPED, MESH_RUN_STARTING, MESH_RUN_RUNNING, MESH_RUN_STOPPING};
    for (size_t i = 0; i < sizeof(from) / sizeof(from[0]); i++) {
        mesh_run_t u = {from[i], MESH_WHY_USER};
        CHECK(mesh_run_event(&u, MESH_EV_UPDATE));
        CHECK(is(u, MESH_RUN_STOPPED, MESH_WHY_UPDATE));
        CHECK(!mesh_run_event(&u, MESH_EV_START));
        CHECK(!mesh_run_event(&u, MESH_EV_SEEN_UP));
        CHECK(!mesh_run_event(&u, MESH_EV_DONE_OK)); // a start job that ends after the update changes nothing
        CHECK(!mesh_run_event(&u, MESH_EV_DONE_FAIL));
        CHECK(!mesh_run_event(&u, MESH_EV_UPDATE));
        CHECK(mesh_run_wants_radio(u));
    }
}

static void texts(void) {
    char       l1[32], l2[32];
    mesh_run_t r = MESH_RUN_INIT;
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)));
    CHECK(strcmp(l1, "Not running") == 0 && strcmp(l2, "Enter to start") == 0);
    CHECK(mesh_run_tile_text(r, false, l1, sizeof(l1), l2, sizeof(l2)));
    CHECK(strcmp(l2, "Needs the LoRa radio") == 0);
    CHECK(strcmp(mesh_run_console_text(r), "stopped (autostart off)") == 0);
    CHECK(strcmp(mesh_run_action_label(r), "Start MeshCore") == 0 && strcmp(mesh_run_action_value(r), "Enter") == 0);
    CHECK(strcmp(mesh_run_grid_text(r, true), "MeshCore is not running. Enter starts it.") == 0);

    mesh_run_event(&r, MESH_EV_START);
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)));
    CHECK(strcmp(l1, "Starting...") == 0 && l2[0] == '\0');
    CHECK(strcmp(mesh_run_console_text(r), "starting...") == 0);
    CHECK(strcmp(mesh_run_action_label(r), "Starting...") == 0 && mesh_run_action_value(r)[0] == '\0');
    CHECK(strcmp(mesh_run_grid_text(r, true), "Starting MeshCore...") == 0);

    mesh_run_event(&r, MESH_EV_DONE_OK);
    CHECK(!mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2))); // the usual tile
    CHECK(mesh_run_console_text(r) == NULL);
    CHECK(mesh_run_grid_text(r, true)[0] == '\0');
    CHECK(strcmp(mesh_run_action_label(r), "Stop MeshCore") == 0 && strcmp(mesh_run_action_value(r), "Enter") == 0);

    mesh_run_event(&r, MESH_EV_STOP);
    CHECK(strcmp(mesh_run_action_label(r), "Stopping...") == 0);
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)) && strcmp(l1, "Stopping...") == 0);
    mesh_run_event(&r, MESH_EV_DONE_OK);
    CHECK(strcmp(mesh_run_console_text(r), "stopped") == 0);

    mesh_run_event(&r, MESH_EV_UPDATE);
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)) && strcmp(l2, "Stopped for update") == 0);
    CHECK(strcmp(mesh_run_console_text(r), "stopped for update") == 0);
    CHECK(strcmp(mesh_run_action_label(r), "Stopped for update") == 0 && mesh_run_action_value(r)[0] == '\0');

    CHECK(strcmp(mesh_run_autostart_text(true), "on") == 0 && strcmp(mesh_run_autostart_text(false), "off") == 0);
    // A tile line cut to its buffer stays terminated
    char tiny[4];
    mesh_run_t s = MESH_RUN_INIT;
    CHECK(mesh_run_tile_text(s, true, tiny, sizeof(tiny), l2, sizeof(l2)) && strcmp(tiny, "Not") == 0);
}

int main(void) {
    machine();
    texts();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_run_test ok\n");
    return 0;
}
