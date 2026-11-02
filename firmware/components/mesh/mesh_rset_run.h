#pragma once
// A repeater's settings (#117) on the badge: drives mesh_rset.h's rs_t through the remote CLI (mesh_remote_start /
// mesh_remote_get, one request at a time, never waiting on MeshCore), for the settings screen (meshcore_rset.c, from
// an LVGL timer) and for the console's read-only "fw mesh remote <who> settings" (a short-lived job task).
#include "mesh.h"
#include "mesh_rset.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    rs_t        rs;
    rr_t        rr; // the last copy of the request
    uint8_t     target[6];
    uint32_t    seq;         // our CLI request in flight, 0 for none
    uint32_t    not_ours_ms; // when a reply that is not ours was first seen: ours may still follow it
    char const *why;         // why the last command got no answer or did not go out (static text), NULL for none
} rs_run_t;

void rs_run_init(rs_run_t *x, uint8_t const prefix[6]);
// One step: hands a finished answer to rs, then sends the next command rs has. true when the rows or the outcome may
// have changed. Any task; never blocks.
bool rs_run_step(rs_run_t *x);
bool rs_run_busy(rs_run_t const *x);

// The console (mesh_task.cpp's "fw mesh remote"): reads every row of the repeater (logs in first with the remembered
// password when this badge is not logged in to it) on a job task and prints each row as it comes. Never sends a set.
void mesh_rset_console_read(uint8_t const prefix[6]);
// The rows of the last console read
void mesh_rset_console_show(void);

#ifdef __cplusplus
}
#endif
