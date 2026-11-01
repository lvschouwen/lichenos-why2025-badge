#pragma once
// The primitives the history journal's encryption needs (#98), behind one C face for mesh_hist.c. Two builds of
// mesh_hist_crypto.cpp, chosen at compile time:
//   badge (ESP_PLATFORM): mbedTLS 4's PSA Crypto API, which IDF routes to the P4's AES and SHA engines (its esp_aes,
//                         esp_sha and esp_mac PSA drivers; the legacy mbedtls_aes_* calls are private in mbedTLS 4)
//   host tests:           rweather's portable Crypto library (SHA256, AES128), as MeshCore builds it
// mh_selftest() runs the same known-answer vectors on both: the host tests run it on the portable code, the badge at
// boot on the hardware path, so a difference between the two shows instead of making journals unreadable.
// Every call returns false when the primitive failed (the badge's crypto can run out of key slots or memory); the
// caller then treats its output as garbage.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SHA-256 of a || b
bool mh_sha256(uint8_t out[32], void const *a, size_t a_len, void const *b, size_t b_len);
// HMAC-SHA-256 over a || b || c (any part may be NULL with length 0)
bool mh_hmac(uint8_t out[32], uint8_t const *key, size_t key_len, void const *a, size_t a_len, void const *b,
             size_t b_len, void const *c, size_t c_len);
// AES-128 in counter mode, in place. Counter block: iv[0..12) then the block index as a 32-bit big-endian number from
// 0 (the same as a 128-bit big-endian counter starting at iv | 0: a record never has 2^32 blocks, so no carry)
bool mh_aes128_ctr(uint8_t const key[16], uint8_t const iv[12], uint8_t *buf, size_t len);
// The same from block index first (the known-answer test of NIST SP 800-38A F.5.1 starts at fcfdfeff)
bool mh_aes128_ctr_at(uint8_t const key[16], uint8_t const iv[12], uint32_t first, uint8_t *buf, size_t len);
// Overwrites a secret (not optimised away)
void mh_wipe(void *p, size_t len);
// "hardware (PSA)" or "portable"
char const *mh_backend(void);
// Known-answer tests (mesh_hist_selftest.c): 0 when all pass, else the number of the first that failed
int mh_selftest(void);

#ifdef __cplusplus
}
#endif
