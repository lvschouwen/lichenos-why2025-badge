// Bookkeeping for a file staged in pieces (#37): the SHA-256 of every piece as it was verified, so a later whole-file
// mismatch can say which pieces no longer match. Pure, host-tested in host_tests/ota_pieces_test.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define OTA_PIECES_MAX 1024 // more pieces than this are simply not recorded (the whole-file check still runs)

typedef struct {
    uint32_t offset;
    uint32_t len;
    uint8_t  sha[32];
} ota_piece_t;

typedef struct {
    ota_piece_t *v;
    size_t       n;
    size_t       cap;
} ota_pieces_t;

// Round n up to a multiple of align (a power of two)
size_t ota_round_up(size_t n, size_t align);

// Record a verified piece. Returns false when out of memory or past OTA_PIECES_MAX.
int ota_pieces_add(ota_pieces_t *p, size_t offset, size_t len, uint8_t const sha[32]);

// Forget all pieces
void ota_pieces_clear(ota_pieces_t *p);

// Re-hash every recorded piece in data and write the indices of those that differ to bad (at most max).
// Returns how many differ in total.
size_t ota_pieces_check(ota_pieces_t const *p, uint8_t const *data, size_t *bad, size_t max);

// A piece streamed into flash (#37) must start on an erase sector, and end on one unless it is the last piece, so
// erasing it for a resend never takes a neighbour's bytes with it
int ota_piece_flash_ok(size_t offset, size_t len, size_t total, size_t sector);

// Reads n bytes at off of some store (the OTA slot) into buf; returns 0 on success
typedef int (*ota_read_fn)(void *ctx, size_t off, uint8_t *buf, size_t n);

// SHA-256 of len bytes at off of a store, read in scratch_len-sized chunks. Returns 0, or -1 when a read fails.
int ota_hash_range(ota_read_fn read, void *ctx, size_t off, size_t len, uint8_t *scratch, size_t scratch_len,
                   uint8_t digest[32]);

// ota_pieces_check over a store: indices of the pieces that differ (an unreadable piece counts as differing)
size_t ota_pieces_check_read(ota_pieces_t const *p, ota_read_fn read, void *ctx, uint8_t *scratch, size_t scratch_len,
                             size_t *bad, size_t max);
