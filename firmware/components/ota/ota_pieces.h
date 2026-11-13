// Bookkeeping for a file staged in pieces (#37): the SHA-256 of every piece as it was verified, so a later whole-file
// mismatch can say which pieces no longer match. Pure, host-tested in host_tests/ota_pieces_test.c.
#pragma once

#include "ota_proto.h"

#include <stdbool.h>
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

// Round n up to a multiple of align (a power of two); 0 when align is not one or the result would not fit a size_t
size_t ota_round_up(size_t n, size_t align);

// Record a verified piece. Returns false when out of memory, past OTA_PIECES_MAX, or when offset + len does not fit
// the 32-bit record.
int ota_pieces_add(ota_pieces_t *p, size_t offset, size_t len, uint8_t const sha[32]);

// Forget all pieces
void ota_pieces_clear(ota_pieces_t *p);

// Re-hash every recorded piece in data (data_len bytes) and write the indices of those that differ to bad (at most
// max); a piece reaching past data_len counts as differing and is not read. Returns how many differ in total.
size_t ota_pieces_check(ota_pieces_t const *p, uint8_t const *data, size_t data_len, size_t *bad, size_t max);

// A piece streamed into flash (#37) must start on an erase sector, and end on one unless it is the last piece, so
// erasing it for a resend never takes a neighbour's bytes with it. False for sector 0 or a piece past total.
int ota_piece_flash_ok(size_t offset, size_t len, size_t total, size_t sector);

// Why the server refuses a piece request (X-Offset present) before touching anything (#211). The server answers them
// in the order of ota_piece_headers, then its own path checks for /file, then ota_piece_bounds.
typedef enum {
    OTA_PIECE_OK,
    OTA_PIECE_MISSING,     // 400: Content-Length, X-Content-SHA256, X-Total-Size or X-File-SHA256 absent
    OTA_PIECE_BAD_DEFLATE, // 400: compressed without X-Piece-Size, or X-Piece-Size on a plain piece (#298)
    OTA_PIECE_TOO_LARGE,   // 413: X-Total-Size past the route's limit
    OTA_PIECE_FIRST_PLAIN, // 400: a compressed firmware piece at offset 0 (its image header is checked before erasing)
    OTA_PIECE_BLOATED,     // 413: a compressed piece longer than any stream of X-Piece-Size bytes needs to be
    OTA_PIECE_UNALIGNED, // 400: a firmware piece inside the file that is not whole erase sectors (ota_piece_flash_ok)
    OTA_PIECE_PAST_END,  // 400: X-Offset + Content-Length runs past X-Total-Size
} ota_piece_verdict_t;

// The bytes a piece adds to the file: X-Piece-Size when compressed, else Content-Length (the server reads it only once
// ota_piece_headers and ota_piece_bounds passed)
long long ota_piece_len(ota_request_t const *req);
// The most bytes a zlib stream of len decoded bytes may take (stored blocks of incompressible data are a little longer)
long long ota_deflate_bound(long long len);
// The header checks: the fields a piece needs, the size limit max, and for firmware the sector rule
ota_piece_verdict_t ota_piece_headers(ota_request_t const *req, long long max, size_t sector);
// The piece lies inside the file; no sum of network numbers can overflow here
ota_piece_verdict_t ota_piece_bounds(ota_request_t const *req);

// The upload in pieces under way, as the server holds it
typedef struct {
    bool        active;
    ota_route_t route;
    size_t      total;
    size_t      got; // bytes verified so far: the offset the next piece must have
    char const *path;
    char const *file_sha256;
} ota_piece_expect_t;

// Whether req (with path, "" for routes without one) is the next piece of that upload: same route, file, size, whole
// file hash, and the offset where the last verified piece ended
bool ota_piece_is_next(ota_piece_expect_t const *e, ota_request_t const *req, char const *path);

// Whether a streamed image may be installed (#298): every byte of it verified in pieces (got == total) and the SHA-256
// of what the slot holds (slot_sha, lowercase hex) equal to the whole file's (file_sha). Nothing else switches the boot
// slot, compressed pieces or not.
bool ota_whole_file_ok(size_t got, size_t total, char const *slot_sha, char const *file_sha);

// Reads n bytes at off of some store (the OTA slot) into buf; returns 0 on success
typedef int (*ota_read_fn)(void *ctx, size_t off, uint8_t *buf, size_t n);

// SHA-256 of len bytes at off of a store, read in scratch_len-sized chunks. Returns 0, or -1 when a read fails.
int ota_hash_range(ota_read_fn read, void *ctx, size_t off, size_t len, uint8_t *scratch, size_t scratch_len,
                   uint8_t digest[32]);

// ota_pieces_check over a store: indices of the pieces that differ (an unreadable piece counts as differing)
size_t ota_pieces_check_read(ota_pieces_t const *p, ota_read_fn read, void *ctx, uint8_t *scratch, size_t scratch_len,
                             size_t *bad, size_t max);
