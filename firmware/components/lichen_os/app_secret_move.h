#pragma once
// The one-time move of an app's plain secrets into its secrets class (#288): MeshCore kept each repeater password as a
// plain setting "pw<hex>" before; the move makes it "~pw<hex>" (app_secret.h). Pure, host-tested with injected storage
// failures and a power cut after every write and erase (host_tests/app_secret_move_test.c); app_secret_boot.c is the
// NVS glue, run once per boot before anything reads a setting of an app.
//
// Per key: read the plain value; unless the stored secret already holds the same (a cut after the write), write it,
// read it back and compare; only then erase the plain key. While the plain copy exists it is the value that counts
// (a stored one that differs is written over: a bad read-back last time, or a password set after a move that failed).
// NVS writes one entry atomically, so a power cut at any point leaves the plain value, the secret, or both: never
// neither, and the plain one is never erased before its secret read back equal. The record (APP_SECRET_MOVED_KEY) is
// written only after every key moved, so a cut or a failure means the next boot runs the move again; with the record
// there it never runs again.
// Values are never logged and the buffers that held one are wiped.

#include "app_secret.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SECRET_MOVED_KEY "~:moved" // u8 in the app's namespace; ':' is never in a prefix, so never a secret's name
#define APP_SECRET_MOVE_BATCH 16       // plain keys moved per boot (stack: 16 B each); more wait for the next boot

typedef struct {
    void *env;
    // The plain keys of the namespace that start with prefix (not the stored ones), up to cap; *more when there were
    // more than cap. Returns how many.
    size_t (*list)(void *env, char const *prefix, char (*keys)[APP_SECRET_STORED_MAX], size_t cap, bool *more);
    // A string value: false when missing, unreadable or longer than cap - 1 (out is then not to be used)
    bool (*get)(void *env, char const *key, char *out, size_t cap);
    bool (*set)(void *env, char const *key, char const *value); // written and committed
    bool (*erase)(void *env, char const *key);                  // gone and committed (also when it was not there)
    bool (*moved_get)(void *env);                               // the record is there
    bool (*moved_set)(void *env);                               // written and committed
} app_secret_store_t;

typedef enum {
    APP_SECRET_MOVE_ALREADY,  // the record was there: nothing done
    APP_SECRET_MOVE_DEFERRED, // another settings migration ran this boot: nothing done, the next boot moves
    APP_SECRET_MOVE_DONE,     // every key moved (or none to move), the record written
    APP_SECRET_MOVE_PARTIAL,  // something failed or more keys wait: the next boot runs again
} app_secret_move_t;

typedef struct {
    unsigned moved;  // plain keys gone with their secret in place
    unsigned failed; // plain keys still there (a read, write, read-back or erase failed)
} app_secret_move_count_t;

// One app's move. other_migration: another settings migration ran in this boot (lichen_nvs's to encryption), and two
// never share a boot. count may be NULL.
app_secret_move_t app_secret_move(app_secret_store_t const *s, char const *prefix, bool other_migration,
                                  app_secret_move_count_t *count);

#ifdef __cplusplus
}
#endif
