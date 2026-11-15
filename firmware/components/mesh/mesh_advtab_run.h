#pragma once

#include "mesh_advtab.h"

#ifdef __cplusplus
extern "C" {
#endif

// The advert table's memory and its snapshot file (#469): on the SD card when one is mounted (every 15 minutes if the
// table changed), else on /storage (every 6 hours), and at every stop of MeshCore (a clean shutdown, a planned
// restart, an update). The snapshot holds only what nodes broadcast (their adverts): nothing secret goes on the card.
// Loaded once at boot from the newest valid file of the two places; a missing or damaged one is an empty table.
// The per-node files of older firmware (<files_dir>/bl/) are read in once and removed after the first good snapshot.

#define MESH_ADVTAB_CAP 4000 // #472's contact cap with a card; without one the contact table holds fewer

// Before the mesh task exists: the table (PSRAM), filled from the snapshot and the old files. NULL: no memory
mesh_advtab_t *mesh_advtab_run_boot(char const *files_dir);
// With the mesh lock: a snapshot is due (the caller prunes the table, then calls mesh_advtab_run_take_locked)
bool mesh_advtab_run_due(void);
// With the mesh lock: copies the table for mesh_advtab_run_write
void mesh_advtab_run_take_locked(void);
// On the mesh task, without the lock: writes what was taken; a few old per-node files go per call while any are left
void mesh_advtab_run_write(void);
// With the mesh lock, at a stop or MeshCore's own reboot: a changed table is written now, whatever the interval; after
// mesh_advtab_run_wiped the snapshots are removed first (a factory reset restarts before the next write)
void mesh_advtab_run_save_locked(void);
// MeshCore's files were erased (factory reset): the table is empty and the snapshots go at the next write
void mesh_advtab_run_wiped(void);
// One console line: where the snapshot lives, counts, the last write and errors
void mesh_advtab_run_console(void);

#ifdef __cplusplus
}
#endif
