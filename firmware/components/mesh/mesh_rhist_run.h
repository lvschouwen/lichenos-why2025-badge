#pragma once
// Repeater status history (#168) on the badge: mesh_rhist.h's table in PSRAM under its own mutex, the poll that runs
// from the mesh loop, the setting in the app's settings (a.meshcore "rh_poll", like the advert interval: what is
// switched on stays on after a restart, and at a start the asks are staggered) and the console's read-out.
#include "mesh.h"
#include "mesh_rhist.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once a second from the mesh loop (mesh lock held; it takes the table's mutex for at most 20 ms and the remote
// request's lock): the first call makes the table and reads the setting; then it ends our request, starts the next
// due one through mesh_remote_start_background, and writes the setting again while an earlier write failed
void mesh_rhist_tick(void);
// A screen that asks a repeater (Log in, Status, Command, Settings) opened (true) or went (false): no poll starts
// while one is open. Any task.
void mesh_rhist_hold(bool open);

// One repeater's slot, copied (the screen's view, a PSRAM buffer: rh_slot_t is about 3 KB), with the table's
// schedule state and the time it was taken. false when MeshCore did not make the table yet.
typedef struct {
    bool      have;     // the repeater has a slot (on, or off with its history)
    rh_slot_t slot;
    bool      asking;   // our request to it runs
    size_t    on_count; // repeaters switched on
    bool      unsaved;  // the setting is not written yet (the tick retries)
    uint32_t  now_s;
} rh_view_t;
bool mesh_rhist_view(uint8_t const prefix[RH_PREFIX], rh_view_t *out);

// Whether the repeater is switched on, without the copy: for a row that shows it every second
bool mesh_rhist_is_on(uint8_t const prefix[RH_PREFIX]);

// rh_needs_confirm on the table (true when the table cannot be read and on is asked)
bool mesh_rhist_needs_confirm(uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min);

typedef enum { RHS_OK, RHS_FULL, RHS_NOT_RUNNING, RHS_NOT_SAVED, RHS_BAD, RHS_BUSY } rhs_result_t;
// On (with an interval from RH_INTERVAL_CHOICES), off, or a new interval; saved at once. Any task but the mesh task
// (it waits for the table's lock). NOT_SAVED: the change holds; the mesh task writes the setting again every 30 s until
// it is written, so an off is never undone by a restart once a write succeeds.
// from (#290): who switches it; a console line's asks flood with the console scope from then on (saved with it)
rhs_result_t mesh_rhist_set(uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min, mesh_origin_t from);
// The console scope was changed (#290): every repeater a console line switched on goes off, saved. How many (0 also
// before the table is made: then the first tick does it).
size_t       mesh_rhist_off_console(void);
// The contact was removed: its slot and its entry in the setting go (a failed write is retried as for mesh_rhist_set)
void mesh_rhist_forget(uint8_t const prefix[RH_PREFIX]);
// The text of a result for a screen or the console
char const *mesh_rhist_result_text(rhs_result_t r);

// "fw mesh history" (prefix NULL): every slot and its schedule; "fw mesh history <who>": also its samples;
// "<who> on [minutes] YES" / "<who> off" / "<who> interval <minutes> [YES]" (YES when it shortens the interval)
void mesh_rhist_console(uint8_t const *prefix, char const *verb);

#ifdef __cplusplus
}
#endif
