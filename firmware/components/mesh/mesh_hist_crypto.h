#pragma once
// The primitives the history journal's encryption needs (#98), from rweather's Crypto library that MeshCore already
// links (SHA-256 and software AES-128): a C face for mesh_hist.c. mesh_hist_crypto.cpp is built without ESP32, so
// AES is the library's portable code on the badge and on the host alike.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SHA-256 of a || b
void mh_sha256(uint8_t out[32], void const *a, size_t a_len, void const *b, size_t b_len);
// HMAC-SHA-256 over a || b || c (any part may be NULL with length 0)
void mh_hmac(uint8_t out[32], uint8_t const *key, size_t key_len, void const *a, size_t a_len, void const *b,
             size_t b_len, void const *c, size_t c_len);
// AES-128 in counter mode, in place: the counter block is iv[0..12) followed by the block index (big-endian, from 0)
void mh_aes128_ctr(uint8_t const key[16], uint8_t const iv[12], uint8_t *buf, size_t len);
// Overwrites a secret (not optimised away)
void mh_wipe(void *p, size_t len);

#ifdef __cplusplus
}
#endif
