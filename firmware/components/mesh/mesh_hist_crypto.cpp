#include "mesh_hist_crypto.h"

#include <string.h>

extern "C" bool mh_aes128_ctr(uint8_t const key[16], uint8_t const iv[12], uint8_t *buf, size_t len) {
    return mh_aes128_ctr_at(key, iv, 0, buf, len);
}

#ifdef ESP_PLATFORM

// ---- badge: PSA Crypto (mbedTLS 4), hardware AES and SHA through IDF's drivers ------------------------------------

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#define CTR_CHUNK 64 // bytes per psa_cipher_update into a separate output buffer (no in-place assumption)

static psa_key_id_t import_key(psa_key_type_t type, psa_algorithm_t alg, psa_key_usage_t usage, uint8_t const *key,
                               size_t len) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t         id   = 0;
    psa_set_key_type(&attr, type);
    psa_set_key_algorithm(&attr, alg);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_bits(&attr, len * 8);
    if (psa_import_key(&attr, key, len, &id) != PSA_SUCCESS) {
        id = 0;
    }
    psa_reset_key_attributes(&attr);
    return id;
}

extern "C" bool mh_sha256(uint8_t out[32], void const *a, size_t a_len, void const *b, size_t b_len) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t               n  = 0;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
              psa_hash_update(&op, (uint8_t const *)a, a_len) == PSA_SUCCESS &&
              psa_hash_update(&op, (uint8_t const *)b, b_len) == PSA_SUCCESS &&
              psa_hash_finish(&op, out, 32, &n) == PSA_SUCCESS && n == 32;
    if (!ok) {
        psa_hash_abort(&op);
    }
    return ok;
}

extern "C" bool mh_hmac(uint8_t out[32], uint8_t const *key, size_t key_len, void const *a, size_t a_len,
                        void const *b, size_t b_len, void const *c, size_t c_len) {
    psa_algorithm_t const alg = PSA_ALG_HMAC(PSA_ALG_SHA_256);
    psa_key_id_t          id  = import_key(PSA_KEY_TYPE_HMAC, alg, PSA_KEY_USAGE_SIGN_MESSAGE, key, key_len);
    if (!id) {
        return false;
    }
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    size_t              n  = 0;
    bool ok = psa_mac_sign_setup(&op, id, alg) == PSA_SUCCESS &&
              (!a_len || psa_mac_update(&op, (uint8_t const *)a, a_len) == PSA_SUCCESS) &&
              (!b_len || psa_mac_update(&op, (uint8_t const *)b, b_len) == PSA_SUCCESS) &&
              (!c_len || psa_mac_update(&op, (uint8_t const *)c, c_len) == PSA_SUCCESS) &&
              psa_mac_sign_finish(&op, out, 32, &n) == PSA_SUCCESS && n == 32;
    if (!ok) {
        psa_mac_abort(&op);
    }
    psa_destroy_key(id);
    return ok;
}

extern "C" bool mh_aes128_ctr_at(uint8_t const key[16], uint8_t const iv[12], uint32_t first, uint8_t *buf,
                                 size_t len) {
    psa_key_id_t id = import_key(PSA_KEY_TYPE_AES, PSA_ALG_CTR, PSA_KEY_USAGE_ENCRYPT, key, 16);
    if (!id) {
        return false;
    }
    // PSA counts the whole 16-byte block as one big-endian number: from iv | first that is iv | block index, as long
    // as the index does not wrap (a record has at most 16 blocks; the test vector starts 4 blocks below 2^32 - 2^8)
    uint8_t                block[16], tmp[CTR_CHUNK];
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    memcpy(block, iv, 12);
    block[12] = (uint8_t)(first >> 24);
    block[13] = (uint8_t)(first >> 16);
    block[14] = (uint8_t)(first >> 8);
    block[15] = (uint8_t)first;
    bool ok = psa_cipher_encrypt_setup(&op, id, PSA_ALG_CTR) == PSA_SUCCESS &&
              psa_cipher_set_iv(&op, block, sizeof(block)) == PSA_SUCCESS;
    for (size_t done = 0; ok && done < len;) {
        size_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp), got = 0;
        ok       = psa_cipher_update(&op, buf + done, n, tmp, sizeof(tmp), &got) == PSA_SUCCESS && got == n;
        if (ok) {
            memcpy(buf + done, tmp, n);
            done += n;
        }
    }
    size_t fin = 0;
    ok         = ok && psa_cipher_finish(&op, tmp, sizeof(tmp), &fin) == PSA_SUCCESS && fin == 0;
    if (!ok) {
        psa_cipher_abort(&op);
    }
    mbedtls_platform_zeroize(tmp, sizeof(tmp));
    psa_destroy_key(id);
    return ok;
}

extern "C" void mh_wipe(void *p, size_t len) {
    mbedtls_platform_zeroize(p, len);
}

extern "C" char const *mh_backend(void) {
    return "hardware (PSA)";
}

#else

// ---- host tests: rweather's portable Crypto ------------------------------------------------------------------------

#include <AES.h>
#include <Crypto.h>
#include <SHA256.h>

extern "C" bool mh_sha256(uint8_t out[32], void const *a, size_t a_len, void const *b, size_t b_len) {
    SHA256 sha;
    sha.update(a, a_len);
    sha.update(b, b_len);
    sha.finalize(out, 32);
    return true;
}

extern "C" bool mh_hmac(uint8_t out[32], uint8_t const *key, size_t key_len, void const *a, size_t a_len,
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
    return true;
}

extern "C" bool mh_aes128_ctr_at(uint8_t const key[16], uint8_t const iv[12], uint32_t first, uint8_t *buf,
                                 size_t len) {
    AES128  aes;
    uint8_t block[16], stream[16];
    aes.setKey(key, 16);
    memcpy(block, iv, 12);
    for (uint32_t index = first; len; index++) {
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
    return true;
}

extern "C" void mh_wipe(void *p, size_t len) {
    clean(p, len);
}

extern "C" char const *mh_backend(void) {
    return "portable";
}

#endif
