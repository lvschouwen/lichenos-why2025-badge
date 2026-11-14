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
        mesh_run_t u = {from[i], MESH_WHY_USER, false};
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

// The OS pauses MeshCore for the LoRa spectrum (#188)
static void paused(void) {
    char       l1[32], l2[32];
    mesh_run_t r = MESH_RUN_INIT;
    mesh_run_event(&r, MESH_EV_START);
    mesh_run_event(&r, MESH_EV_DONE_OK);
    CHECK(mesh_run_event(&r, MESH_EV_PAUSE) && r.paused);
    CHECK(!mesh_run_event(&r, MESH_EV_PAUSE));
    CHECK(mesh_run_event(&r, MESH_EV_STOP));
    CHECK(mesh_run_event(&r, MESH_EV_DONE_OK));
    CHECK(is(r, MESH_RUN_STOPPED, MESH_WHY_USER) && r.paused); // the pause survives the stop it asked for
    CHECK(!mesh_run_wants_radio(r));                           // the status bar dims, not red
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)));
    CHECK(strcmp(l1, "Paused") == 0 && strcmp(l2, "LoRa spectrum is open") == 0);
    CHECK(strcmp(mesh_run_grid_text(r, true), "MeshCore is paused: LoRa spectrum is open.") == 0);
    CHECK(strcmp(mesh_run_console_text(r), "paused (LoRa spectrum is open)") == 0);
    CHECK(strcmp(mesh_run_action_label(r), "Paused") == 0);
    CHECK(!mesh_run_event(&r, MESH_EV_START)); // refused while paused
    CHECK(mesh_run_event(&r, MESH_EV_UNPAUSE) && !r.paused);
    CHECK(!mesh_run_event(&r, MESH_EV_UNPAUSE));
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)) && strcmp(l1, "Not running") == 0);
    CHECK(mesh_run_event(&r, MESH_EV_START)); // resume clears the reason
    CHECK(mesh_run_event(&r, MESH_EV_DONE_OK) && !r.paused && r.state == MESH_RUN_RUNNING);

    // An update beats a pause, and clears it
    mesh_run_event(&r, MESH_EV_PAUSE);
    mesh_run_event(&r, MESH_EV_UPDATE);
    CHECK(!r.paused && !mesh_run_event(&r, MESH_EV_PAUSE));
    CHECK(strcmp(mesh_run_console_text(r), "stopped for update") == 0);
}

static void texts(void) {
    char       l1[32], l2[32];
    mesh_run_t r = MESH_RUN_INIT;
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)));
    CHECK(strcmp(l1, "Not running") == 0 && strcmp(l2, "Enter to start") == 0);
    CHECK(mesh_run_tile_text(r, false, l1, sizeof(l1), l2, sizeof(l2)));
    CHECK(strcmp(l2, "Needs the LoRa radio") == 0);
    CHECK(strcmp(mesh_run_console_text(r), "stopped (autostart off)") == 0);
    CHECK(strcmp(mesh_run_action_label(r), "Start MeshCore") == 0 && mesh_run_action_value(r)[0] == '\0');
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
    CHECK(strcmp(mesh_run_action_label(r), "Stop MeshCore") == 0 && mesh_run_action_value(r)[0] == '\0');

    mesh_run_event(&r, MESH_EV_STOP);
    CHECK(strcmp(mesh_run_action_label(r), "Stopping...") == 0);
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)) && strcmp(l1, "Stopping...") == 0);
    mesh_run_event(&r, MESH_EV_DONE_OK);
    CHECK(strcmp(mesh_run_console_text(r), "stopped") == 0);

    mesh_run_event(&r, MESH_EV_UPDATE);
    CHECK(mesh_run_tile_text(r, true, l1, sizeof(l1), l2, sizeof(l2)) && strcmp(l2, "Stopped for update") == 0);
    CHECK(strcmp(mesh_run_console_text(r), "stopped for update") == 0);
    CHECK(strcmp(mesh_run_action_label(r), "Stopped for update") == 0 && mesh_run_action_value(r)[0] == '\0');

    CHECK(strcmp(mesh_run_autostart_text(true), "On") == 0 && strcmp(mesh_run_autostart_text(false), "Off") == 0);
    // A tile line cut to its buffer stays terminated
    char tiny[4];
    mesh_run_t s = MESH_RUN_INIT;
    CHECK(mesh_run_tile_text(s, true, tiny, sizeof(tiny), l2, sizeof(l2)) && strcmp(tiny, "Not") == 0);
}

// #377: a remote Enter that would start MeshCore is refused; which states count
static void enter_starts(void) {
    mesh_run_t r = MESH_RUN_INIT;
    CHECK(mesh_run_enter_starts(r, true));
    CHECK(!mesh_run_enter_starts(r, false)); // not on its grid or home tile
    r.paused = true;
    CHECK(mesh_run_enter_starts(r, true)); // fails safe
    r = (mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_UPDATE, false};
    CHECK(!mesh_run_enter_starts(r, true)); // stopped for an update: Enter starts nothing
    r.state = MESH_RUN_RUNNING;
    CHECK(!mesh_run_enter_starts(r, true));
    r.state = MESH_RUN_STARTING;
    CHECK(!mesh_run_enter_starts(r, true));
    r.state = MESH_RUN_STOPPING;
    CHECK(!mesh_run_enter_starts(r, true));
}

// Every state x why x paused x event (#212: recorded from the switch before it was split into handlers). Per event
// four digits: changed, then the state, why and paused after it
static char const *const EVERY_OUTCOME[4 * 4 * 2] = {
    "1100 0000 0000 0000 1030 1200 0000 1001 0000", // state 0 why 0 paused 0
    "0001 0001 0001 0001 1030 1201 0001 0001 1000", // state 0 why 0 paused 1
    "1110 0010 0010 0010 1030 1210 0010 1011 0010", // state 0 why 1 paused 0
    "0011 0011 0011 0011 1030 1211 0011 0011 1010", // state 0 why 1 paused 1
    "1120 0020 0020 0020 1030 1220 0020 1021 0020", // state 0 why 2 paused 0
    "0021 0021 0021 0021 1030 1221 0021 0021 1020", // state 0 why 2 paused 1
    "0030 0030 0030 0030 0030 0030 0030 0030 0030", // state 0 why 3 paused 0
    "0031 0031 0031 0031 0031 0031 0031 0031 1030", // state 0 why 3 paused 1
    "0100 0100 1200 1020 1030 0100 0100 1101 0100", // state 1 why 0 paused 0
    "0101 0101 1201 1021 1030 0101 0101 0101 1100", // state 1 why 0 paused 1
    "0110 0110 1210 1020 1030 0110 0110 1111 0110", // state 1 why 1 paused 0
    "0111 0111 1211 1021 1030 0111 0111 0111 1110", // state 1 why 1 paused 1
    "0120 0120 1220 1020 1030 0120 0120 1121 0120", // state 1 why 2 paused 0
    "0121 0121 1221 1021 1030 0121 0121 0121 1120", // state 1 why 2 paused 1
    "0130 0130 1230 1020 1030 0130 0130 1131 0130", // state 1 why 3 paused 0
    "0131 0131 1231 1021 1030 0131 0131 0131 1130", // state 1 why 3 paused 1
    "0200 1300 0200 0200 1030 0200 1020 1201 0200", // state 2 why 0 paused 0
    "0201 1301 0201 0201 1030 0201 1020 0201 1200", // state 2 why 0 paused 1
    "0210 1310 0210 0210 1030 0210 1020 1211 0210", // state 2 why 1 paused 0
    "0211 1311 0211 0211 1030 0211 1020 0211 1210", // state 2 why 1 paused 1
    "0220 1320 0220 0220 1030 0220 1020 1221 0220", // state 2 why 2 paused 0
    "0221 1321 0221 0221 1030 0221 1020 0221 1220", // state 2 why 2 paused 1
    "0230 1330 0230 0230 1030 0230 1020 1231 0230", // state 2 why 3 paused 0
    "0231 1331 0231 0231 1030 0231 1020 0231 1230", // state 2 why 3 paused 1
    "0300 0300 1010 1200 1030 0300 0300 1301 0300", // state 3 why 0 paused 0
    "0301 0301 1011 1201 1030 0301 0301 0301 1300", // state 3 why 0 paused 1
    "0310 0310 1010 1210 1030 0310 0310 1311 0310", // state 3 why 1 paused 0
    "0311 0311 1011 1211 1030 0311 0311 0311 1310", // state 3 why 1 paused 1
    "0320 0320 1010 1220 1030 0320 0320 1321 0320", // state 3 why 2 paused 0
    "0321 0321 1011 1221 1030 0321 0321 0321 1320", // state 3 why 2 paused 1
    "0330 0330 1010 1230 1030 0330 0330 1331 0330", // state 3 why 3 paused 0
    "0331 0331 1011 1231 1030 0331 0331 0331 1330", // state 3 why 3 paused 1
};

static void every_outcome(void) {
    size_t k = 0;
    for (int s = 0; s < 4; s++) {
        for (int w = 0; w < 4; w++) {
            for (int p = 0; p < 2; p++, k++) {
                char got[9 * 5];
                size_t n = 0;
                for (int e = 0; e <= MESH_EV_UNPAUSE; e++) {
                    mesh_run_t r = {(mesh_run_state_t)s, (mesh_run_why_t)w, p != 0};
                    bool       c = mesh_run_event(&r, (mesh_run_ev_t)e);
                    n += (size_t)snprintf(got + n, sizeof(got) - n, "%d%d%d%d%s", c, (int)r.state, (int)r.why, r.paused,
                                          e < MESH_EV_UNPAUSE ? " " : "");
                }
                if (strcmp(got, EVERY_OUTCOME[k]) != 0) {
                    printf("FAIL state %d why %d paused %d: got \"%s\", want \"%s\"\n", s, w, p, got, EVERY_OUTCOME[k]);
                    failures++;
                }
            }
        }
    }
}

int main(void) {
    every_outcome();
    enter_starts();
    machine();
    paused();
    texts();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_run_test ok\n");
    return 0;
}
