#pragma once

#include "mesh_ctab.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The contact list's two files (#472, mesh_ctab.h): MeshCore's "contacts3" on /storage and, when a card is mounted,
// /sd/lichen/meshcore/contacts.bin with the whole list, encrypted. With this badge's card file in use the table takes
// MESH_CTAB_CAP_CARD contacts and /storage keeps the core set; otherwise it takes MESH_CTAB_CAP_INTERNAL and /storage
// keeps everything, as before. Nothing but contacts goes on the card: no identity, no channel secret, no setting.
//
// Every save of contacts3 that MeshCore makes is parked by the file shim (#425) and comes here after the mesh lock:
// this is the one writer of both files. The card is written every MESH_ADVTAB_CARD_S while the table changes and at
// every stop; /storage at once when the owner changed something, else every MESH_ADVTAB_INTERNAL_S and at every stop.
// A save that only moved last_advert_timestamp / lastmod stays parked (still what the read view serves) and is written
// every MESH_CTAB_SAVE_INTERVAL_S and at every stop (#429); a record added or removed or any other field writes at once.
// A crash loses what changed since the last save of the place it was in; a node heard again comes back by itself.
//
// Threading: the _locked functions run with the mesh lock held; mesh_ctab_run_saved and mesh_ctab_run_work on the mesh
// task after the unlock (file work); the console line, the state and the owner's request from any task.

typedef enum {
    MESH_CTAB_ON_STORAGE,     // no card, no key, or the card failed: the list lives on /storage
    MESH_CTAB_ON_CARD,        // this badge's card file is in use
    MESH_CTAB_CARD_LEFT_ALONE, // the card holds another badge's contact file, or one that cannot be read
} mesh_ctab_place_t;

// How many contacts the table takes right now (BadgeMesh::getMaxContacts)
int mesh_ctab_run_cap(void);
mesh_ctab_place_t mesh_ctab_run_place(void);
// The owner's "Use this card for contacts", at the badge: the mesh task moves the file it left alone aside
// (contacts.old, contacts.old.1; never overwritten) and starts a new one. Only a request: the place changes when done.
void mesh_ctab_run_use_card(void);
// Why the last such request did nothing, NULL when it worked or none was made
char const *mesh_ctab_run_use_card_error(void);

// Once per mesh loop, with the lock: merges a part of a card file that was read, and makes MeshCore save its contacts
// when the card is due (the save is parked and comes to mesh_ctab_run_saved after the unlock)
void mesh_ctab_run_tick_locked(void);
// With the lock, right after a parked save was taken: what the files' rules need from the table and the chat store
void mesh_ctab_run_taken_locked(void);
// With the lock: the owner removed this contact / changed the list by hand (an import)
void mesh_ctab_run_deleted_locked(uint8_t const *pub);
void mesh_ctab_run_owner_change(void);
// With the lock, at a stop or MeshCore's own reboot, in place of a direct save: the contacts go to /storage and to the
// card now. After mesh_ctab_run_wiped nothing is saved and this badge's card file is removed. final: the key is wiped.
void mesh_ctab_run_stop_locked(bool final);
// MeshCore's files were erased (factory reset)
void mesh_ctab_run_wiped(void);
// Once per mesh loop, after the unlock: a card put in or pulled, the owner's request, the deleted-keys file, notices
void mesh_ctab_run_work(void);
// One console line: where the list lives, counts, the last writes and errors
void mesh_ctab_run_console(void);

#ifdef __cplusplus
}

#include "FS.h"

class BadgeMesh;
// Before the mesh task exists, after begin() loaded contacts3: the deleted keys are applied, and a card file of this
// badge is merged into the table. prv: the node's private key (not kept; NULL: the card is not used)
void mesh_ctab_run_boot(BadgeMesh *n, char const *files_dir, uint8_t const *prv, size_t prv_len);
// The mesh task, after the unlock: a parked contacts3 save (h is emptied); contacts: the table's count, for the log
void mesh_ctab_run_saved(fs_shim_held_t *h, int contacts);
#endif
