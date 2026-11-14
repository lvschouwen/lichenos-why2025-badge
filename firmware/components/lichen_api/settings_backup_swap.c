#include "settings_backup_swap.h"

bool sbs_save(sbs_ops_t const *ops, void *ctx, char const *buf, size_t len) {
    if (!ops->write(ctx, SBS_NEW, buf, len)) {
        ops->remove(ctx, SBS_NEW); // partial: best effort, a leftover fails validation on the read anyway
        return false;
    }
    // From here NEW is complete and valid. An OLD left by an earlier cut is older than MAIN, and rename would refuse it
    if (!ops->remove(ctx, SBS_OLD)) {
        return false;
    }
    bool had_main = ops->rename(ctx, SBS_MAIN, SBS_OLD); // false also when there was no MAIN yet (first save)
    if (!ops->rename(ctx, SBS_NEW, SBS_MAIN)) {
        // MAIN is gone or still there; either way NEW (or OLD) is readable. Do not undo: that would be another window
        return false;
    }
    if (had_main) {
        ops->remove(ctx, SBS_OLD); // best effort: a leftover is older than MAIN and never preferred
    }
    return true;
}

sbs_name_t sbs_choose(sbs_probe_fn probe, void *ctx, bool *saw_file) {
    *saw_file = false;
    for (sbs_name_t n = SBS_MAIN; n < SBS_COUNT; n++) {
        sbs_state_t s = probe(ctx, n);
        if (s == SBS_VALID) {
            return n;
        }
        if (s == SBS_INVALID) {
            *saw_file = true;
            if (n == SBS_MAIN) { // a swap never leaves a bad MAIN (it only appears by rename): this is an edit or a bad card,
                break;           // and falling back to an older copy would load what the person did not mean
            }
        }
    }
    return SBS_COUNT;
}
