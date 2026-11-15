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

// Whether a snapshot is due: the table changed since the last one and its interval has passed (on_card picks it)
bool mesh_advtab_due(bool changed, bool on_card, uint32_t now_s, uint32_t last_save_s);
// The key in an old per-node file's name (16 hex digits, either case); false for any other name
bool mesh_advtab_key_from_name(char const *name, uint8_t key[MESH_ADVTAB_KEY]);

#ifdef __cplusplus
}
#endif
