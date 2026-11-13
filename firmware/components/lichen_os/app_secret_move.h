#pragma once
// The move of an app's plain secrets into its secrets class (#288): MeshCore kept each repeater password as a plain
// setting "pw<hex>" before; the move makes it "~pw<hex>" (app_secret.h). Pure, host-tested with injected storage
// failures and a power cut after every write and erase (host_tests/app_secret_move_test.c); app_secret_boot.c is the
// NVS glue, run at every boot before anything reads a setting of an app.
//
// It runs whenever a plain key under the prefix exists: the first boot of this image, and again after a rollback to an
// older image saved a retyped password plainly. The record (APP_SECRET_MOVED_KEY) only says the move was told once (so
// a boot with nothing to move logs nothing); it never stops a move.
//
// Per key: read the plain value; unless the stored secret already holds the same (a cut after the write), write it,
// read it back and compare; only then erase the plain key. While the plain copy exists it is the value that counts
// (a stored one that differs is written over: a bad read-back last time, a password set after a move that failed, or
// the older value after a rollback). NVS writes one entry atomically, so a power cut at any point leaves the plain
// value, the secret, or both: never neither, and the plain one is never erased before its secret read back equal.
// A plain key that can never be a secret (a name too long for the mark, a value over APP_SECRET_VALUE_MAX: only from
// crafted storage) is erased and counted as dropped.
//
// No loop: one bounded batch (APP_SECRET_MOVE_BATCH) per boot, and every key in it ends moved, dropped or failed (a
// storage error). Without storage errors the plain keys only shrink, so the boots end with none left; with them each
// boot retries one batch. Values are never logged and the buffers that held one are wiped.

#include "app_secret.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SECRET_MOVED_KEY "~:moved" // u8 in the app's namespace; ':' is never in a prefix, so never a secret's name
#define APP_SECRET_MOVE_BATCH 16       // plain keys moved per boot (stack: 16 B each); more wait for the next boot

typedef enum {
    APP_SECRET_GET_OK,
    APP_SECRET_GET_NONE,     // missing or unreadable
    APP_SECRET_GET_TOO_LONG, // there, but longer than cap - 1
} app_secret_get_t;

typedef struct {
    void *env;
    // The plain keys of the namespace that start with prefix (not the stored ones), up to cap, in keys and *n; *more
    // when there were more than cap. False when the listing failed (the end of the entries is not a failure).
    bool (*list)(void *env, char const *prefix, char (*keys)[APP_SECRET_STORED_MAX], size_t cap, size_t *n,
                 bool *more);
    app_secret_get_t (*get)(void *env, char const *key, char *out, size_t cap); // out only valid for OK
    bool (*set)(void *env, char const *key, char const *value);                 // written and committed
    bool (*erase)(void *env, char const *key); // gone and committed (also when it was not there)
    bool (*moved_get)(void *env);              // the record is there
    bool (*moved_set)(void *env);              // written and committed
} app_secret_store_t;

typedef enum {
    APP_SECRET_MOVE_ALREADY,  // nothing under the prefix and the record there: nothing done, nothing to say
    APP_SECRET_MOVE_DEFERRED, // another settings migration ran this boot: nothing done, the next boot moves
    APP_SECRET_MOVE_DONE,     // every key moved or dropped (or none to move), the record there
    APP_SECRET_MOVE_PARTIAL,  // something failed or more keys wait: the next boot runs again
} app_secret_move_t;

typedef struct {
    unsigned moved;   // plain keys gone with their secret in place
    unsigned dropped; // plain keys erased that could never be a secret
    unsigned failed;  // plain keys still there (a read, write, read-back or erase failed); a failed listing counts one
} app_secret_move_count_t;

// One app's move. other_migration: another settings migration ran in this boot (lichen_nvs's to encryption), and two
// never share a boot. count may be NULL.
app_secret_move_t app_secret_move(app_secret_store_t const *s, char const *prefix, bool other_migration,
                                  app_secret_move_count_t *count);

#ifdef __cplusplus
}
#endif
