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
