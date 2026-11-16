#pragma once
// Drops the remembered repeater passwords whose contact is gone (#413). A rollback to an image from before #288 deletes
// a contact and erases only the old plain key, so the stored secret outlives it. Pure, host-tested in
// host_tests/mesh_pw_sweep_test.c; meshcore_run.c supplies the storage and the contact table. Fails closed: a secret is
// erased only when the contact table says "not there" for a key that rr_pw_key made; a busy table, a key of another
// shape or a failed listing leaves everything as it is. Keys are secrets' names: never logged, buffers wiped.

#include "app_secret.h"
#include "mesh_remote.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_SWEEP_BATCH 16 // secrets looked at per sweep (stack: 15 B each); more wait for the next one

typedef enum { PW_CONTACT_THERE, PW_CONTACT_GONE, PW_CONTACT_UNKNOWN } pw_contact_t; // UNKNOWN: busy, not running

typedef struct {
    void *env;
    size_t       (*list)(void *env, char (*keys)[APP_SECRET_KEY_BUF], size_t cap); // app_secret_list
    pw_contact_t (*contact)(void *env, uint8_t const prefix[MF_PREFIX]);
    bool         (*erase)(void *env, char const *key); // app_secret_erase
} pw_sweep_io_t;

// Returns how many secrets were erased; *left is set to the number of listed secrets still there (kept contacts,
// unknown state, unparsable keys, failed erases). Either out pointer may be NULL.
size_t pw_sweep(pw_sweep_io_t const *io, size_t *left);

#ifdef __cplusplus
}
#endif
