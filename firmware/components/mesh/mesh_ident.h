#pragma once
// The stored MeshCore identity checked at load (#80): MeshCore saves its key pair from a PSRAM copy without a
// checksum, so a bit flip there would be persisted silently. The public key derived from the stored private key must
// equal the stored public key; when it does not, the badge must not transmit (anything it signed or encrypted would be
// wrong, and its adverts would carry a key nobody can talk to). Never a reason to regenerate or rewrite the identity.
// Pure (MeshCore's Ed25519 code only): host-tested in mesh_ident_test.c.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_IDENT_PRV_SIZE 64 // MeshCore's expanded Ed25519 private key: scalar, then the signing prefix
#define MESH_IDENT_PUB_SIZE 32

typedef enum {
    MESH_IDENT_UNCHECKED, // not loaded yet
    MESH_IDENT_OK,
    MESH_IDENT_MISMATCH,
} mesh_ident_t;

// Derives the public key from prv and compares it with pub. Only the scalar half of prv goes into the public key, so
// a flip in the signing prefix (prv[32..63]) is not seen here: it gives wrong signatures under a right key.
mesh_ident_t mesh_ident_check(uint8_t const prv[MESH_IDENT_PRV_SIZE], uint8_t const pub[MESH_IDENT_PUB_SIZE]);
// Transmitting is allowed only after a check that passed
bool         mesh_ident_tx_allowed(mesh_ident_t state);
// For "fw mesh": "ok", "MISMATCH, TX blocked", "not checked"
char const  *mesh_ident_text(mesh_ident_t state);

#ifdef __cplusplus
}
#endif
