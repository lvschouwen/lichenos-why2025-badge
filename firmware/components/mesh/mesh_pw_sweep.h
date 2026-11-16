#pragma once
// Drops the remembered repeater passwords whose contact is gone (#413). A rollback to an image from before #288 deletes
// a contact and erases only the old plain key, so the stored secret outlives it. Pure, host-tested in
// host_tests/mesh_pw_sweep_test.c; meshcore_run.c supplies the storage and the contact table. Fails closed: a secret is
// erased only when the table was loaded completely, the contact table says "not there" for a key that rr_pw_key made;
// a busy table (the first UNKNOWN ends the sweep, so a contended lock costs one wait), a key of another shape or a
// failed listing leaves everything as it is (an erase cannot be undone).
// The namespace is walked in batches of PW_SWEEP_BATCH (the stack bound), skipping what was kept. Keys are secrets'
// names: never logged, buffers wiped.

#include "app_secret.h"
#include "mesh_remote.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_SWEEP_BATCH   16  // secrets looked at per batch (stack: 15 B each)
#define PW_SWEEP_BATCHES 64  // batches per sweep: far more secrets than NVS can hold, so the walk always ends
#define PW_CONTACTS_REC  152 // bytes of a record in MeshCore's contacts3 (DataStore::loadContacts)

typedef enum { PW_CONTACT_THERE, PW_CONTACT_GONE, PW_CONTACT_UNKNOWN } pw_contact_t; // UNKNOWN: busy, not running

typedef struct {
    void *env;
    // app_secret_list: skip, then up to cap keys; *more when others follow
    size_t       (*list)(void *env, char (*keys)[APP_SECRET_KEY_BUF], size_t cap, size_t skip, bool *more);
    pw_contact_t (*contact)(void *env, uint8_t const prefix[MF_PREFIX]);
    bool         (*erase)(void *env, char const *key); // app_secret_erase
} pw_sweep_io_t;

// True when the table loaded from contacts3 is known complete: the file is a whole number of records, holds at least
// one, the table holds exactly that many, and the table is not full (a full table stops reading the file)
bool pw_load_complete(long file_bytes, long records_in_table, long table_cap);

// trusted: the contact table is known complete (pw_load_complete and the list lives on /storage); false does nothing.
// Returns how many secrets were erased; *left is set to the number kept or left unchecked up to where the sweep ended
// (kept contacts, unknown state, unparsable keys, failed erases).
// *stopped_early is set when a busy or not-running contact table ended the sweep.
// The out pointers may be NULL.
size_t pw_sweep(pw_sweep_io_t const *io, bool trusted, size_t *left, bool *stopped_early);

#ifdef __cplusplus
}
#endif
