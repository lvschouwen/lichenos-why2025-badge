#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Advert blobs on their way to /storage (#257). For every advert it accepts MeshCore saves the raw packet (the copy
// "share contact" sends on) as its own file, /bl/<first 8 key bytes>: open, write, close, two or three 4 KB
// wear-levelled flash sector rewrites (data, directory entry; the FAT too for a new node), 200-340 ms, all of it inside
// MeshCore's loop with the mesh lock held. Here the blob waits in RAM instead, and the mesh task writes it after the
// unlock, one per loop: when no packet came in that loop, or once it has waited MESH_BLOBQ_MAX_AGE_MS. Reads see the
// queued copy first, so nothing can tell it is not on flash yet; a power cut loses at most the blobs of the adverts of
// the last MESH_BLOBQ_MAX_AGE_MS (the next advert of that node brings it back). Pure logic, host-tested
// (host_tests/mesh_blobq_test.c).

#define MESH_BLOBQ_KEY        8   // DataStore names the file after the key's first 8 bytes: that is the identity here
#define MESH_BLOBQ_DATA       255 // DataStore's limit (a whole LoRa packet)
#define MESH_BLOBQ_CAP        32  // full: the caller writes the blob itself, as before
#define MESH_BLOBQ_MAX_AGE_MS 5000

typedef struct {
    uint8_t  key[MESH_BLOBQ_KEY];
    uint8_t  data[MESH_BLOBQ_DATA];
    uint8_t  len;      // 0: a free slot
    uint32_t since_ms; // when the oldest version not yet on flash was queued (a newer one does not postpone it)
    uint32_t seq;      // a new one for every put: a write of an older version does not take a newer one off
} mesh_blobq_entry_t;

typedef struct {
    mesh_blobq_entry_t e[MESH_BLOBQ_CAP];
    uint32_t           next_seq;
} mesh_blobq_t;

void mesh_blobq_init(mesh_blobq_t *q);
// Queues the blob, replacing a queued one with the same key. false (nothing queued): no room, a key shorter than
// MESH_BLOBQ_KEY, or a length of 0 or above MESH_BLOBQ_DATA; the caller then writes it itself.
bool mesh_blobq_put(mesh_blobq_t *q, uint8_t const *key, size_t key_len, uint8_t const *data, size_t len,
                    uint32_t now_ms);
// The queued blob's length, copied to dest (MESH_BLOBQ_DATA bytes of room); 0 when none is queued for this key
uint8_t mesh_blobq_get(mesh_blobq_t const *q, uint8_t const *key, size_t key_len, uint8_t *dest);
// The blob to write now, copied to out, the one that waited longest: any when quiet (no packet in this loop), else
// only one that has waited MESH_BLOBQ_MAX_AGE_MS. It stays queued (and readable) until mesh_blobq_done.
bool mesh_blobq_take(mesh_blobq_t const *q, uint32_t now_ms, bool quiet, mesh_blobq_entry_t *out);
// out of mesh_blobq_take was written (or is not wanted): off the queue, unless a newer version was put since
void mesh_blobq_done(mesh_blobq_t *q, mesh_blobq_entry_t const *written);
void   mesh_blobq_clear(mesh_blobq_t *q);
size_t mesh_blobq_count(mesh_blobq_t const *q);

#ifdef __cplusplus
}
#endif
