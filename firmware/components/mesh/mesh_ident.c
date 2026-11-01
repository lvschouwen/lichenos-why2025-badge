#include "mesh_ident.h"

#include "ed_25519.h"

#include <string.h>

mesh_ident_t mesh_ident_check(uint8_t const prv[MESH_IDENT_PRV_SIZE], uint8_t const pub[MESH_IDENT_PUB_SIZE]) {
    unsigned char derived[MESH_IDENT_PUB_SIZE];
    ed25519_derive_pub(derived, prv);
    // the whole key compared, not stopping at the first difference
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(derived); i++) {
        diff |= (uint8_t)(derived[i] ^ pub[i]);
    }
    return diff == 0 ? MESH_IDENT_OK : MESH_IDENT_MISMATCH;
}

mesh_ident_t mesh_ident_after_load(bool file_unreadable, uint8_t const prv[MESH_IDENT_PRV_SIZE],
                                   uint8_t const pub[MESH_IDENT_PUB_SIZE]) {
    return file_unreadable ? MESH_IDENT_UNREADABLE : mesh_ident_check(prv, pub);
}

bool mesh_ident_tx_allowed(mesh_ident_t state) {
    return state == MESH_IDENT_OK;
}

char const *mesh_ident_text(mesh_ident_t state) {
    switch (state) {
    case MESH_IDENT_OK:
        return "ok";
    case MESH_IDENT_MISMATCH:
        return "MISMATCH, TX blocked";
    case MESH_IDENT_UNREADABLE:
        return "file unreadable, TX blocked";
    default:
        return "not checked";
    }
}
