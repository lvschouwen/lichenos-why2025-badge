#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The last raw advert of every contact (#469). MeshCore keeps it for two rare actions (sharing a contact zero-hop,
// exporting one) and replaces it on every advert it accepts: a cache of what the mesh sends again by itself. It was
// one file per node on /storage (#257's queue wrote them after the mesh lock), which cost a LittleFS metadata pair per
// handful of nodes and a flash write per advert. Here it is a table in PSRAM; mesh_advtab_run writes a snapshot of it
// rarely. A crash loses the adverts since the last snapshot, and each comes back with that node's next advert.
// Pure logic, host-tested (host_tests/mesh_advtab_test.c). Not thread safe: the caller holds the mesh lock.

#define MESH_ADVTAB_KEY  8   // the key's first 8 bytes are the identity, as DataStore's file names were
#define MESH_ADVTAB_MIN  100 // an advert is at least a key, a timestamp and a signature: shorter is not kept
#define MESH_ADVTAB_DATA 255 // a whole LoRa packet
#define MESH_ADVTAB_HEAD 24  // the snapshot's header
#define MESH_ADVTAB_REC  (MESH_ADVTAB_KEY + 1) // a record's key and length in a snapshot, before its bytes

#define MESH_ADVTAB_CARD_S     (15 * 60)      // a changed table goes to the SD card this often (owner, #469)
#define MESH_ADVTAB_INTERNAL_S (6 * 60 * 60)  // and to /storage this often when there is no card (flash wear)

typedef struct {
    uint8_t *keys;    // cap * MESH_ADVTAB_KEY, apart from the data: a lookup walks 8 bytes a slot, not 264
    uint8_t *lens;    // cap; 0: a free slot
    uint8_t *data;    // cap * MESH_ADVTAB_DATA
    size_t   cap;
    size_t   top;     // slots at and above it were never used
    size_t   count;
    uint32_t changes; // one more for every put, removal and load: a snapshot is due when it differs from the saved one
    uint32_t refused; // puts that found the table full
} mesh_advtab_t;

// The memory one table of cap slots needs (one block; PSRAM on the badge)
size_t mesh_advtab_bytes(size_t cap);
void   mesh_advtab_init(mesh_advtab_t *t, void *mem, size_t cap);
// Keeps the blob, replacing the node's earlier one. false: a key shorter than MESH_ADVTAB_KEY, a length outside
// MESH_ADVTAB_MIN..MESH_ADVTAB_DATA, or no free slot (counted in refused)
bool    mesh_advtab_put(mesh_advtab_t *t, uint8_t const *key, size_t key_len, uint8_t const *data, size_t len);
// The blob's length, copied to dest (MESH_ADVTAB_DATA bytes of room); 0 when the node has none
uint8_t mesh_advtab_get(mesh_advtab_t const *t, uint8_t const *key, size_t key_len, uint8_t *dest);
bool    mesh_advtab_remove(mesh_advtab_t *t, uint8_t const *key, size_t key_len);
void    mesh_advtab_clear(mesh_advtab_t *t);
// Drops every blob whose node keep() no longer knows (a removed or overwritten contact); how many went
size_t  mesh_advtab_prune(mesh_advtab_t *t, bool (*keep)(uint8_t const key[MESH_ADVTAB_KEY], void *ctx), void *ctx);

// ---- the snapshot: a header, then per blob its key, its length and its bytes. Little-endian --------------------------
//   header  "LADV", u16 format 1, u16 0, u32 count, u32 body bytes, u32 crc32 of the body (zlib's), u32 generation
// The bytes a snapshot of the table takes now
size_t mesh_advtab_pack_size(mesh_advtab_t const *t);
// Writes the snapshot without its checksum (a copy, short enough for under the mesh lock); the bytes used, 0 when buf
// is too small. mesh_advtab_seal then fills the checksum in, outside the lock.
size_t mesh_advtab_pack(mesh_advtab_t const *t, uint32_t generation, uint8_t *buf, size_t cap);
void   mesh_advtab_seal(uint8_t *buf, size_t len);
// A whole, undamaged snapshot: the header, the length, the checksum and every record's length add up. *generation is
// set when it is one. Trailing bytes after the snapshot are not accepted.
bool   mesh_advtab_valid(uint8_t const *buf, size_t len, uint32_t *generation);
// Replaces the table's content with a valid snapshot's; the blobs loaded (those beyond cap are left out)
size_t mesh_advtab_unpack(mesh_advtab_t *t, uint8_t const *buf, size_t len);

#define MESH_ADVTAB_RETRY_S 60 // a forced snapshot (the takeover's first) that failed is tried again this often

// What the snapshot's clock knows. All times are uptime seconds.
typedef struct {
    bool     changed;          // the table differs from the last snapshot written
    bool     on_card;          // an SD card is mounted
    bool     forced;           // the takeover's first snapshot: does not wait for its interval
    bool     internal_written; // /storage took a snapshot since boot (last_internal_s is valid)
    uint32_t now_s;
    uint32_t last_try_s;       // boot, or the last attempt, whether it worked or not
    uint32_t last_internal_s;
} mesh_advtab_clock_t;
// Whether a snapshot is tried now: the table changed and the interval of its place has passed since the last attempt
// (a failure waits an interval too); a forced one waits only MESH_ADVTAB_RETRY_S, never less.
bool mesh_advtab_due(mesh_advtab_clock_t const *c);
// Whether /storage may take the snapshot now (flash wear): at a stop always, forced always, else at most every
// MESH_ADVTAB_INTERNAL_S since the last one it took
bool mesh_advtab_internal_ok(mesh_advtab_clock_t const *c, bool at_stop);
// The key in an old per-node file's name (16 hex digits, either case); false for any other name
bool mesh_advtab_key_from_name(char const *name, uint8_t key[MESH_ADVTAB_KEY]);

// ---- the one-time takeover of older firmware's per-node files, in slices (#469, paced like #471's walks) ------------------
// A walk of a folder of a few hundred files is too long for one go on the mesh task (core 0 starves IDLE, the task
// watchdog fires), so the caller runs mesh_advtab_walk_step once per loop. The directory cursor lives in the caller's
// ctx and survives between calls; the walk's progress is in mesh_advtab_walk_t (zero it to begin).
typedef struct {
    bool     done;       // the folder was read to its end: the old files may go once a snapshot holds the result
    bool     failed;     // the folder read failed (not its end): the old files must stay; not to be called again
    uint32_t names;      // names seen so far
    uint32_t taken_over; // nodes added to the table
} mesh_advtab_walk_t;
typedef struct {
    // The next name of the folder, NULL at its end; valid until the next call
    char const *(*next)(void *ctx);
    // Reads the file called name into data (MESH_ADVTAB_DATA bytes of room); its length, 0 when it cannot be read
    size_t (*read)(void *ctx, char const *name, uint8_t *data);
    // The table is the mesh lock's (not thread safe): held around every lookup and put, never around next or read
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    // Optional. Asked after each name: true when this call's time budget is spent, the step then returns at once
    // (the walk resumes next call). Never asked before the first name, so every call makes progress.
    bool (*spent)(void *ctx);
    // Optional. Asked when next returned NULL: false when that was an error and not the end of the folder
    bool (*end_clean)(void *ctx);
} mesh_advtab_walk_ops_t;
// At most max_names names this call (fewer when ops->spent says the budget is gone, never fewer than one): each one
// that is a node key the table does not have is read and put. true once the end was reached (w->done) or the folder
// failed (w->failed); then it is not to be called again.
bool mesh_advtab_walk_step(mesh_advtab_t *t, mesh_advtab_walk_t *w, mesh_advtab_walk_ops_t const *ops, void *ctx,
                           size_t max_names);

#ifdef __cplusplus
}
#endif
