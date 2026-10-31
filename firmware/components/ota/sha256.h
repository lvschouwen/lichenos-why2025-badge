// SHA-256 (FIPS 180-4), small and dependency free; apps can't reach the firmware's mbedtls
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t  block[64];
    size_t   block_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, void const *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_LEN]);
