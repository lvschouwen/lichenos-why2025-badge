#include "mesh_pw_sweep.h"

#include "secure_wipe.h"

size_t pw_sweep(pw_sweep_io_t const *io, size_t *left) {
    char   keys[PW_SWEEP_BATCH][APP_SECRET_KEY_BUF];
    size_t erased = 0, kept = 0;
    size_t n = io->list(io->env, keys, PW_SWEEP_BATCH);
    if (n > PW_SWEEP_BATCH) {
        n = 0; // a listing that overran its cap is not to be trusted
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t prefix[MF_PREFIX];
        if (rr_pw_key_prefix(keys[i], prefix) && io->contact(io->env, prefix) == PW_CONTACT_GONE &&
            io->erase(io->env, keys[i])) {
            erased++;
        } else {
            kept++;
        }
    }
    secure_wipe(keys, sizeof(keys));
    if (left) {
        *left = kept;
    }
    return erased;
}
