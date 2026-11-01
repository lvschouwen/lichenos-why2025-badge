#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Whether MeshCore runs, and the texts that say so (#103): start at boot can be off, and MeshCore starts and stops on
// demand (its home tile, its grid, its Settings page, "fw mesh start|stop"). Pure, no ESP headers
// (host_tests/mesh_run_test.c); meshcore_run.c keeps one under a spinlock and runs the starts and stops on a job task.

typedef enum { MESH_RUN_STOPPED, MESH_RUN_STARTING, MESH_RUN_RUNNING, MESH_RUN_STOPPING } mesh_run_state_t;

// Why it is stopped
typedef enum {
    MESH_WHY_AUTOSTART_OFF, // not started at boot (the setting); nothing asked for it since
    MESH_WHY_USER,          // stopped on request
    MESH_WHY_FAILED,        // a start failed, or it stopped without being asked
    MESH_WHY_UPDATE,        // stopped for a firmware update: final, no start before the restart
} mesh_run_why_t;

typedef enum {
    MESH_EV_START,      // asked to start: accepted only when stopped (and not for an update)
    MESH_EV_STOP,       // asked to stop: accepted only when running
    MESH_EV_DONE_OK,    // the start or stop that was asked for finished
    MESH_EV_DONE_FAIL,  // it did not: a start ends stopped, a stop ends running
    MESH_EV_UPDATE,     // a firmware update stopped it: from any state, final
    MESH_EV_SEEN_UP,    // the mesh task is found running (a sync, e.g. after a stop that finished late)
    MESH_EV_SEEN_DOWN,  // the mesh task is found not running
} mesh_run_ev_t;

typedef struct {
    mesh_run_state_t state;
    mesh_run_why_t   why; // meaningful while stopped
} mesh_run_t;

// The state before anything happened: stopped, start at boot off (boot sends START when the setting is on)
#define MESH_RUN_INIT ((mesh_run_t){MESH_RUN_STOPPED, MESH_WHY_AUTOSTART_OFF})

// Applies ev. True when it changed the state; for START and STOP that means the caller now runs the start or stop and
// reports DONE_OK or DONE_FAIL. False: refused (busy, already there, or stopped for an update), nothing to do.
bool mesh_run_event(mesh_run_t *r, mesh_run_ev_t ev);

// Busy with a start or a stop: a second request is refused
bool mesh_run_busy(mesh_run_t r);

// The home tile while MeshCore is not running: false when it runs (the caller shows its usual tile). lora_ok: the OS's
// LoRa radio is up (else nothing can start). Stopped: "Not running" / "Enter to start".
bool mesh_run_tile_text(mesh_run_t r, bool lora_ok, char *line1, size_t c1, char *line2, size_t c2);

// The grid's line about it while it does not run ("" while running): "Not running. Enter starts MeshCore." ...
char const *mesh_run_grid_text(mesh_run_t r, bool lora_ok);

// The Settings action row: its label ("Stop MeshCore" while running, "Start MeshCore" while stopped, the state while
// busy) and its value (what Enter does now, "" when nothing)
char const *mesh_run_action_label(mesh_run_t r);
char const *mesh_run_action_value(mesh_run_t r);

// The Settings "Start at boot" row's value
char const *mesh_run_autostart_text(bool on);

// "fw mesh" while it does not run, after "MeshCore ": "stopped (autostart off)", "starting..."; NULL while running
char const *mesh_run_console_text(mesh_run_t r);

// The status bar: does MeshCore want the LoRa radio? False only while it is stopped by choice (start at boot off, or
// stopped on request): then the LoRa label is dim, not the red "down"
bool mesh_run_wants_radio(mesh_run_t r);

#ifdef __cplusplus
}
#endif
