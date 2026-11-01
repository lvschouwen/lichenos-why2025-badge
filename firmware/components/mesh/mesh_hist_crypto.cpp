#include "mesh_hist_crypto.h"

#include <AES.h>
#include <Crypto.h>
#include <SHA256.h>

#include <string.h>

extern "C" void mh_sha256(uint8_t out[32], void const *a, size_t a_len, void const *b, size_t b_len) {
    SHA256 sha;
    sha.update(a, a_len);
    sha.update(b, b_len);
    sha.finalize(out, 32);
}

extern "C" void mh_hmac(uint8_t out[32], uint8_t const *key, size_t key_len, void const *a, size_t a_len,
                        void const *b, size_t b_len, void const *c, size_t c_len) {
    SHA256 sha;
    sha.resetHMAC(key, key_len);
    if (a_len) {
        sha.update(a, a_len);
    }
    if (b_len) {
        sha.update(b, b_len);
    }
    if (c_len) {
        sha.update(c, c_len);
    }
    sha.finalizeHMAC(key, key_len, out, 32);
}

extern "C" void mh_aes128_ctr(uint8_t const key[16], uint8_t const iv[12], uint8_t *buf, size_t len) {
    AES128  aes;
    uint8_t block[16], stream[16];
    aes.setKey(key, 16);
    memcpy(block, iv, 12);
    for (uint32_t index = 0; len; index++) {
        block[12] = (uint8_t)(index >> 24);
        block[13] = (uint8_t)(index >> 16);
        block[14] = (uint8_t)(index >> 8);
        block[15] = (uint8_t)index;
        aes.encryptBlock(stream, block);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) {
            buf[i] ^= stream[i];
        }
        buf += n;
        len -= n;
    }
    clean(stream, sizeof(stream));
}

extern "C" void mh_wipe(void *p, size_t len) {
    clean(p, len);
}
