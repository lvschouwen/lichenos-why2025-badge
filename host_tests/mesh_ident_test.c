// Host test: the stored MeshCore identity's check at load (#80). Keys from RFC 8032's first Ed25519 test vector,
// never a real identity.
#include "mesh_ident.h"

#include "ed_25519.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// RFC 8032 section 7.1, TEST 1: the seed ("SECRET KEY") and its public key
static uint8_t const seed[32] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
};
static uint8_t const rfc_pub[32] = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
};

int main(void) {
    // MeshCore stores the expanded private key ed25519_create_keypair makes from the seed
    uint8_t prv[MESH_IDENT_PRV_SIZE], pub[MESH_IDENT_PUB_SIZE];
    ed25519_create_keypair(pub, prv, seed);
    CHECK(memcmp(pub, rfc_pub, sizeof(pub)) == 0); // the library agrees with the RFC

    // a sound pair passes and may transmit
    CHECK(mesh_ident_check(prv, rfc_pub) == MESH_IDENT_OK);
    CHECK(mesh_ident_tx_allowed(MESH_IDENT_OK));
    CHECK(strcmp(mesh_ident_text(MESH_IDENT_OK), "ok") == 0);

    // one bit flipped in the stored public key, at each end and in the middle
    size_t const pub_bits[] = {0, 7, 131, 255};
    for (size_t i = 0; i < sizeof(pub_bits) / sizeof(pub_bits[0]); i++) {
        uint8_t bad[MESH_IDENT_PUB_SIZE];
        memcpy(bad, rfc_pub, sizeof(bad));
        bad[pub_bits[i] / 8] ^= (uint8_t)(1u << (pub_bits[i] % 8));
        CHECK(mesh_ident_check(prv, bad) == MESH_IDENT_MISMATCH);
    }

    // one bit flipped in the private key's scalar (bits the clamping leaves alone)
    size_t const prv_bits[] = {3, 100, 253};
    for (size_t i = 0; i < sizeof(prv_bits) / sizeof(prv_bits[0]); i++) {
        uint8_t bad[MESH_IDENT_PRV_SIZE];
        memcpy(bad, prv, sizeof(bad));
        bad[prv_bits[i] / 8] ^= (uint8_t)(1u << (prv_bits[i] % 8));
        CHECK(mesh_ident_check(bad, rfc_pub) == MESH_IDENT_MISMATCH);
    }

    // an all-zero identity (a file read as zeros) is no pair either
    uint8_t zero_prv[MESH_IDENT_PRV_SIZE] = {0}, zero_pub[MESH_IDENT_PUB_SIZE] = {0};
    CHECK(mesh_ident_check(zero_prv, zero_pub) == MESH_IDENT_MISMATCH);

    // only a passed check transmits
    CHECK(!mesh_ident_tx_allowed(MESH_IDENT_MISMATCH));
    CHECK(!mesh_ident_tx_allowed(MESH_IDENT_UNCHECKED));
    CHECK(strcmp(mesh_ident_text(MESH_IDENT_MISMATCH), "MISMATCH, TX blocked") == 0);
    CHECK(strcmp(mesh_ident_text(MESH_IDENT_UNCHECKED), "not checked") == 0);

    if (failures) {
        printf("mesh_ident_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_ident_test: all passed\n");
    return 0;
}
