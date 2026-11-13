#pragma once

#include "lichen_app.h"
#include "mesh_run.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MeshCore running or not (#103): start at boot (the app's setting "autostart", u8, default 1) and starts and stops on
// demand, each on a short-lived job task so the asker (LVGL, console) never waits on MeshCore. One at a time.

// meshcore_app_register: the context lichen_register_app gave the app, for its settings and its directory
void meshcore_run_init(lichen_ctx_t ctx);
// The app's context (#230 E1b, E1c), for every OS call that takes one (lichen_alloc, lichen_task_start, app_settings_*,
// lichen_fs_*, lichen_badge_set, lichen_notify, ...):
// the one accessor the C and C++ code share. LICHEN_CTX_NONE before meshcore_run_init (logged once; the OS refuses it).
lichen_ctx_t meshcore_ctx(void);
// The app's start_background: registers the console commands, then starts MeshCore when start at boot is on (on this
// task, as before). True when it started or was not meant to.
bool meshcore_run_boot(void);
// The state, brought in line with the mesh task first (a stop that finished late). Quick: any task, any time.
mesh_run_t meshcore_run_get(void);
// Asks for a start (true) or a stop (false) on a job task: NULL when it was taken, else why not ("busy, try again",
// "already running", ...). The screens see the result through meshcore_run_get.
char const *meshcore_run_request(bool start);
// The OS holds the radio for a while (LoRa spectrum, #188): true marks the state paused (tile, grid and "fw mesh" say
// why, starts are refused with MESH_RUN_PAUSED_REFUSAL), false clears it. Mark before the stop, clear before the start.
void meshcore_run_set_paused(bool paused);
// The app's stop_for_update: final
bool meshcore_run_stop_for_update(void);

// The first-open questions (name, region) were answered: the periodic advert (#52) waits for it
bool meshcore_setup_done(void);

// The password remembered for a repeater or room (#51), in the app's settings under the contact's key prefix: the
// stored one into out (true) or empty (false). Never log what these handle.
bool meshcore_pw_get(uint8_t const prefix[6], char *out, size_t cap);
bool meshcore_pw_set(uint8_t const prefix[6], char const *password);
bool meshcore_pw_forget(uint8_t const prefix[6]);

// The one-shot move to 2 B path hashes (#129): done already / now done, in the app's settings
// One-time (#130): the stored routes of another hash size than the setting were forgotten

bool meshcore_autostart(void);
bool meshcore_autostart_set(bool on);

#ifdef __cplusplus
}
#endif
