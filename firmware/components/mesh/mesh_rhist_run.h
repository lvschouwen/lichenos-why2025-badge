#pragma once
// Repeater status history (#168) on the badge: mesh_rhist.h's table in PSRAM under its own spinlock, the poll that runs
// from the mesh loop, the setting in the app's settings (a.meshcore "rh_poll", like the advert interval: what is
// switched on stays on after a restart, and at a start the asks are staggered) and the console's read-out.
#include "mesh_rhist.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once a second from the mesh loop (mesh lock held; it takes no other lock but the table's spinlock and the remote
// request's): the first call makes the table and reads the setting; then it ends our request and starts the next due
// one through mesh_remote_start
void mesh_rhist_tick(void);

// One repeater's slot, copied (the screen's view, a PSRAM buffer: rh_slot_t is about 3 KB), with the table's
// schedule state and the time it was taken. false when MeshCore did not make the table yet.
typedef struct {
    bool      have;     // the repeater has a slot (on, or off with its history)
    rh_slot_t slot;
    bool      asking;   // our request to it runs
    size_t    on_count; // repeaters switched on
    uint32_t  now_s;
} rh_view_t;
bool mesh_rhist_view(uint8_t const prefix[RH_PREFIX], rh_view_t *out);

// Whether the repeater is switched on, without the copy: for a row that shows it every second
bool mesh_rhist_is_on(uint8_t const prefix[RH_PREFIX]);

typedef enum { RHS_OK, RHS_FULL, RHS_NOT_RUNNING, RHS_NOT_SAVED, RHS_BAD } rhs_result_t;
// On (with an interval from RH_INTERVAL_CHOICES), off, or a new interval; saved at once. Any task but the mesh task
// (it writes the setting). NOT_SAVED: the change holds until a restart.
rhs_result_t mesh_rhist_set(uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min);
// The contact was removed: its slot and its entry in the setting go
void mesh_rhist_forget(uint8_t const prefix[RH_PREFIX]);
// The text of a result for a screen or the console
char const *mesh_rhist_result_text(rhs_result_t r);

// "fw mesh history" (prefix NULL): every slot and its schedule; "fw mesh history <who>": also its samples;
// "<who> on [minutes] YES" / "<who> off" / "<who> interval <minutes>"
void mesh_rhist_console(uint8_t const *prefix, char const *verb);

#ifdef __cplusplus
}
#endif
