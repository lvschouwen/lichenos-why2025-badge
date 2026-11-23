// A file for SD0: received in pieces and written straight to the card (#529), instead of staged in PSRAM up to FILE_MAX:
// the piece order, the running SHA-256 that only an accepted piece advances (a resent piece must not hash twice) and the
// size cap from the card's free space. Pure, host-tested in host_tests/ota_sd_stream_test.c.
#pragma once

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One piece is held under the card's lock while it arrives (SD0: has no staging buffer), so it is kept short; this is
// also the piece size tools/badge.sh caps at
#define OTA_SD_PIECE_MAX (256 * 1024)
// The cap when the card's free space is not known, and its ceiling: fseek's offset is a 32-bit long here
#define OTA_SD_TOTAL_MAX 0x7fffffffLL

typedef struct {
    sha256_ctx_t run;   // over the pieces accepted so far
    sha256_ctx_t cand;  // over the piece under way: becomes run only when the piece is accepted
    size_t       got;   // bytes accepted: the offset the next piece must have
    size_t       total; // the file's size
} ota_sd_stream_t;

// The largest file SD0: takes: what the card has free plus what this upload already wrote to it (written: its temp
// file counts against the free space, which every piece lowers; the cap must not shrink under the upload). Unknown free
// space lets the upload go ahead (the room check and the write report it), like the other drive.
long long ota_sd_cap(bool free_known, uint64_t free_bytes, uint64_t written);

// A plain or decoded piece of len bytes may go to the card (0 < len <= OTA_SD_PIECE_MAX)
bool ota_sd_piece_ok(long long len);

void ota_sd_stream_begin(ota_sd_stream_t *s, size_t total);

// Start the piece at offset: the context its bytes are hashed into (a copy of the running one), or NULL when offset is
// not where the next piece goes
sha256_ctx_t *ota_sd_stream_piece(ota_sd_stream_t *s, size_t offset);

// The piece just hashed into the context from ota_sd_stream_piece was verified and written: count it. False when it
// would run past the file's size (nothing changes).
bool ota_sd_stream_accept(ota_sd_stream_t *s, size_t len);

// SHA-256 of everything accepted, only once it is the whole file; false before that
bool ota_sd_stream_final(ota_sd_stream_t const *s, uint8_t digest[SHA256_DIGEST_LEN]);
