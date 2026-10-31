#include "ota_pieces.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

size_t ota_round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

int ota_pieces_add(ota_pieces_t *p, size_t offset, size_t len, uint8_t const sha[32]) {
    if (p->n >= OTA_PIECES_MAX) {
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

size_t ota_pieces_check(ota_pieces_t const *p, uint8_t const *data, size_t *bad, size_t max) {
    size_t differ = 0;
    for (size_t i = 0; i < p->n; i++) {
        sha256_ctx_t ctx;
        uint8_t      digest[SHA256_DIGEST_LEN];
        sha256_init(&ctx);
        sha256_update(&ctx, data + p->v[i].offset, p->v[i].len);
        sha256_final(&ctx, digest);
        if (memcmp(digest, p->v[i].sha, 32) != 0) {
            if (differ < max) {
                bad[differ] = i;
            }
            differ++;
        }
    }
    return differ;
}

int ota_piece_flash_ok(size_t offset, size_t len, size_t total, size_t sector) {
    return len > 0 && offset % sector == 0 && (len % sector == 0 || offset + len == total);
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
