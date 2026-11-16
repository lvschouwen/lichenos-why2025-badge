#include "mesh_pw_sweep.h"

#include "secure_wipe.h"

bool pw_load_complete(long file_bytes, long records_in_table, long table_cap) {
    return file_bytes > 0 && file_bytes % PW_CONTACTS_REC == 0 && records_in_table == file_bytes / PW_CONTACTS_REC &&
           records_in_table < table_cap;
}

// One batch: how many were erased; *kept counts the rest of the batch; *more whether the namespace goes on
static size_t sweep_batch(pw_sweep_io_t const *io, size_t skip, size_t *kept, bool *more) {
    char   keys[PW_SWEEP_BATCH][APP_SECRET_KEY_BUF];
    size_t erased = 0;
    *kept         = 0;
    size_t n      = io->list(io->env, keys, PW_SWEEP_BATCH, skip, more);
    if (n > PW_SWEEP_BATCH) {
        n     = 0; // a listing that overran its cap is not to be trusted
        *more = false;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t prefix[MF_PREFIX];
        if (rr_pw_key_prefix(keys[i], prefix) && io->contact(io->env, prefix) == PW_CONTACT_GONE &&
            io->erase(io->env, keys[i])) {
            erased++;
        } else {
            (*kept)++;
        }
    }
    secure_wipe(keys, sizeof(keys));
    return erased;
}

size_t pw_sweep(pw_sweep_io_t const *io, bool trusted, size_t *left) {
    size_t erased = 0, skip = 0;
    bool   more = trusted;
    for (int b = 0; more && b < PW_SWEEP_BATCHES; b++) {
        size_t kept = 0;
        erased += sweep_batch(io, skip, &kept, &more);
        skip += kept; // erased ones are gone from the listing, the kept ones are what to pass over
    }
    if (left) {
        *left = skip;
    }
    return erased;
}
