// Host test: what update mode does per event (#76)
#include "update_mode.h"

#include <stdio.h>

static int failures;
static int hook_runs;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void hook(void) {
    hook_runs++;
}

#define STOP_CLOSE (UM_ACT_STOP_APPS | UM_ACT_CLOSE_SCREENS)

int main(void) {
    update_state_t s = {0};

    // Old sender: no prepare, update mode starts on the first byte
    CHECK(update_mode_on_begin(&s, true) == STOP_CLOSE);
    CHECK(s.mode == UM_UPDATING);
    CHECK(update_mode_on_begin(&s, true) == UM_ACT_NONE); // a second firmware begin: already stopped
    CHECK(!update_mode_restart_due(&s, true));

    // A file push while updating is refused and changes nothing
    CHECK(!update_mode_file_allowed(&s));
    CHECK(update_mode_on_begin(&s, false) == UM_ACT_REFUSE);
    CHECK(update_mode_on_end(&s, false, false) == UM_ACT_REFUSE);
    CHECK(!s.reboot);

    // Failed firmware: restart after the message; ok afterwards does not cancel it
    CHECK(update_mode_on_end(&s, true, false) == UM_ACT_REBOOT);
    CHECK(s.reboot);
    CHECK(!update_mode_restart_due(&s, false)); // the message is still showing
    CHECK(update_mode_restart_due(&s, true));
    CHECK(update_mode_on_end(&s, true, true) == UM_ACT_NONE);
    CHECK(s.reboot);
    // Only a new firmware begin (or prepare) takes the restart back
    CHECK(update_mode_on_begin(&s, true) == UM_ACT_NONE);
    CHECK(!s.reboot);
    CHECK(update_mode_on_end(&s, true, true) == UM_ACT_NONE); // ok: the sender reboots
    CHECK(!s.reboot);

    // A stalled restart (installed, nobody rebooted it) restarts too
    update_mode_on_restart_stall(&s);
    CHECK(update_mode_restart_due(&s, true));

    // Normal mode: file pushes leave everything alone, failures reboot nothing
    update_state_t n = {0};
    CHECK(update_mode_file_allowed(&n));
    CHECK(update_mode_on_begin(&n, false) == UM_ACT_NONE);
    CHECK(n.mode == UM_NORMAL);
    CHECK(update_mode_on_end(&n, false, false) == UM_ACT_NONE);
    CHECK(update_mode_on_end(&n, true, false) == UM_ACT_NONE); // not in update mode: nothing was stopped
    CHECK(!n.reboot);
    update_mode_on_restart_stall(&n);
    CHECK(!n.reboot);

    // Prepare: stops everything first; the firmware begin that follows has nothing left to do
    update_state_t p = {0};
    CHECK(update_mode_on_prepare(&p, 1000) == STOP_CLOSE);
    CHECK(p.mode == UM_UPDATING && p.prepared);
    CHECK(update_mode_on_prepare(&p, 2000) == UM_ACT_NONE); // asked again: ready again
    CHECK(update_mode_tick(&p, 2000 + UPDATE_PREPARE_TIMEOUT_MS - 1) == UM_ACT_NONE);
    CHECK(update_mode_on_begin(&p, true) == UM_ACT_NONE);
    CHECK(!p.prepared);
    CHECK(update_mode_tick(&p, 2000 + 10 * UPDATE_PREPARE_TIMEOUT_MS) == UM_ACT_NONE); // begun: no timeout

    // Prepare never followed by a begin: a failed update
    update_state_t t = {0};
    update_mode_on_prepare(&t, 5000);
    CHECK(update_mode_tick(&t, 5000 + UPDATE_PREPARE_TIMEOUT_MS) == UM_ACT_REBOOT);
    CHECK(t.reboot && !t.prepared);
    CHECK(update_mode_tick(&t, 5000 + 2 * UPDATE_PREPARE_TIMEOUT_MS) == UM_ACT_NONE); // said once

    // Prepare again after a failure takes the restart back (a retry within the message)
    CHECK(update_mode_on_prepare(&t, 90000) == UM_ACT_NONE);
    CHECK(!t.reboot && t.prepared);

    // Prepare during normal mode with a file push first: fine
    update_state_t f = {0};
    CHECK(update_mode_on_begin(&f, false) == UM_ACT_NONE);
    CHECK(update_mode_on_prepare(&f, 0) == STOP_CLOSE);

    update_mode_run_stop_hook(); // no hook set: nothing happens
    update_mode_set_stop_hook(hook);
    update_mode_run_stop_hook();
    CHECK(hook_runs == 1);

    if (failures) {
        return 1;
    }
    printf("update_mode_test: ok\n");
    return 0;
}
