#include "ota_pieces.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

size_t ota_round_up(size_t n, size_t align) {
    if (align == 0 || (align & (align - 1)) != 0 || n > SIZE_MAX - (align - 1)) {
        return 0;
    }
    return (n + align - 1) & ~(align - 1);
}

int ota_pieces_add(ota_pieces_t *p, size_t offset, size_t len, uint8_t const sha[32]) {
    if (p->n >= OTA_PIECES_MAX || offset > UINT32_MAX || len > UINT32_MAX - offset) {
        return 0;
    }
    if (p->n == p->cap) {
        size_t       cap = p->cap ? p->cap * 2 : 16;
        ota_piece_t *v   = realloc(p->v, cap * sizeof(*v));
        if (!v) {
            return 0;
        }
        p->v   = v;
        p->cap = cap;
    }
    p->v[p->n].offset = (uint32_t)offset;
    p->v[p->n].len    = (uint32_t)len;
    memcpy(p->v[p->n].sha, sha, 32);
    p->n++;
    return 1;
}

void ota_pieces_clear(ota_pieces_t *p) {
    free(p->v);
    memset(p, 0, sizeof(*p));
}

static bool piece_same(ota_piece_t const *pc, uint8_t const *data, size_t data_len) {
    if (pc->offset > data_len || pc->len > data_len - pc->offset) {
        return false;
    }
    sha256_ctx_t ctx;
    uint8_t      digest[SHA256_DIGEST_LEN];
    sha256_init(&ctx);
    sha256_update(&ctx, data + pc->offset, pc->len);
    sha256_final(&ctx, digest);
    return memcmp(digest, pc->sha, 32) == 0;
}

size_t ota_pieces_check(ota_pieces_t const *p, uint8_t const *data, size_t data_len, size_t *bad, size_t max) {
    size_t differ = 0;
    for (size_t i = 0; i < p->n; i++) {
        if (!piece_same(&p->v[i], data, data_len)) {
            if (differ < max) {
                bad[differ] = i;
            }
            differ++;
        }
    }
    return differ;
}

int ota_piece_flash_ok(size_t offset, size_t len, size_t total, size_t sector) {
    if (sector == 0 || len == 0 || offset > total || len > total - offset) {
        return 0;
    }
    return offset % sector == 0 && (len % sector == 0 || len == total - offset);
}

long long ota_piece_len(ota_request_t const *req) {
    return req->deflate ? req->piece_size : req->content_length;
}

// zlib's own deflateBound is len + len/4096 + len/16384 + 13; this leaves room for any sane encoder. len is at most 12
// digits (ota_proto parse_size), so nothing here overflows.
long long ota_deflate_bound(long long len) {
    return len < 0 ? 0 : len + len / 64 + 64;
}

// The sector rule (ota_piece_flash_ok) for a piece inside the file. A piece outside it is ota_piece_bounds's to answer
// ("runs past X-Total-Size"), so it passes here; only inside the file are the numbers below 4 MB and safe to cast.
static bool sector_rule(ota_request_t const *req, size_t sector) {
    return ota_piece_bounds(req) != OTA_PIECE_OK ||
           ota_piece_flash_ok((size_t)req->offset, (size_t)ota_piece_len(req), (size_t)req->total_size, sector);
}

// What compression adds to the header rules (#298); a plain piece without X-Piece-Size passes untouched
static ota_piece_verdict_t deflate_rules(ota_request_t const *req) {
    if (req->deflate != (req->piece_size >= 0) || (req->deflate && req->piece_size == 0)) {
        return OTA_PIECE_BAD_DEFLATE;
    }
    if (req->deflate && req->route == OTA_ROUTE_FIRMWARE && req->offset == 0) {
        return OTA_PIECE_FIRST_PLAIN;
    }
    if (req->deflate && req->content_length > ota_deflate_bound(req->piece_size)) {
        return OTA_PIECE_BLOATED;
    }
    return OTA_PIECE_OK;
}

ota_piece_verdict_t ota_piece_headers(ota_request_t const *req, long long max, size_t sector) {
    if (!req->has_offset || !req->has_sha256 || req->content_length <= 0 || req->total_size <= 0 ||
        !req->has_file_sha256) {
        return OTA_PIECE_MISSING;
    }
    if (req->total_size > max) {
        return OTA_PIECE_TOO_LARGE;
    }
    ota_piece_verdict_t v = deflate_rules(req);
    if (v != OTA_PIECE_OK) {
        return v;
    }
    if (req->route == OTA_ROUTE_FIRMWARE && !sector_rule(req, sector)) {
        return OTA_PIECE_UNALIGNED;
    }
    return OTA_PIECE_OK;
}

ota_piece_verdict_t ota_piece_bounds(ota_request_t const *req) {
    long long len = ota_piece_len(req);
    if (req->offset < 0 || req->content_length <= 0 || len <= 0 || req->total_size <= 0 || len > req->total_size ||
        req->offset > req->total_size - len) {
        return OTA_PIECE_PAST_END;
    }
    return OTA_PIECE_OK;
}

bool ota_piece_is_next(ota_piece_expect_t const *e, ota_request_t const *req, char const *path) {
    return e->active && e->route == req->route && req->total_size > 0 &&
           (unsigned long long)req->total_size == e->total && req->offset >= 0 &&
           (unsigned long long)req->offset == e->got && strcmp(e->path, path) == 0 &&
           strcmp(e->file_sha256, req->file_sha256) == 0;
}

static bool lower_hex64(char const *s) {
    size_t n = strnlen(s, OTA_SHA256_HEX + 1);
    if (n != OTA_SHA256_HEX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool ota_whole_file_ok(size_t got, size_t total, char const *slot_sha, char const *file_sha) {
    return total > 0 && got == total && lower_hex64(slot_sha) && lower_hex64(file_sha) &&
           strcmp(slot_sha, file_sha) == 0;
}

int ota_hash_range(ota_read_fn read, void *ctx, size_t off, size_t len, uint8_t *scratch, size_t scratch_len,
                   uint8_t digest[32]) {
    sha256_ctx_t sha;
    sha256_init(&sha);
    for (size_t at = 0; at < len; at += scratch_len) {
        size_t n = len - at < scratch_len ? len - at : scratch_len;
        if (read(ctx, off + at, scratch, n) != 0) {
            return -1;
        }
        sha256_update(&sha, scratch, n);
    }
    sha256_final(&sha, digest);
    return 0;
}

size_t ota_pieces_check_read(ota_pieces_t const *p, ota_read_fn read, void *ctx, uint8_t *scratch, size_t scratch_len,
                             size_t *bad, size_t max) {
    size_t differ = 0;
    for (size_t i = 0; i < p->n; i++) {
        uint8_t digest[SHA256_DIGEST_LEN];
        if (ota_hash_range(read, ctx, p->v[i].offset, p->v[i].len, scratch, scratch_len, digest) != 0 ||
            memcmp(digest, p->v[i].sha, 32) != 0) {
            if (differ < max) {
                bad[differ] = i;
            }
            differ++;
        }
    }
    return differ;
}
