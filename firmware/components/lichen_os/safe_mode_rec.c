// Crash attribution (#232): the record functions the panic handler runs (safe_mode_policy.h). The linker fragment
// (linker.lf, noflash) puts this whole object in internal RAM: with XIP from PSRAM a flash function is fetched through
// the PSRAM cache, which may be what just faulted. So: no libc calls (the volatile stores keep the compiler from
// turning the loops into memset), no constant tables, nothing outside the record and the id it is given.
#include "safe_mode_policy.h"

#define FNV_BASIS 2166136261u
#define FNV_PRIME 16777619u
#define SUM_SALT  0x5AFE232u // a zeroed record must not sum to 0

static bool id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool safe_mode_id_ok(char const *id) {
    if (!id) {
        return false;
    }
    size_t n = 0;
    while (n < SAFE_MODE_ID_CAP && id[n] && id_char(id[n])) {
        n++;
    }
    return n > 0 && n < SAFE_MODE_ID_CAP && id[n] == '\0';
}

uint32_t safe_mode_sum(safe_mode_rec_t const *r) {
    uint8_t const *p = (uint8_t const *)r;
    uint32_t       h = FNV_BASIS ^ SUM_SALT;
    for (size_t i = 0; i < offsetof(safe_mode_rec_t, check); i++) {
        h = (h ^ p[i]) * FNV_PRIME;
    }
    return h;
}

void safe_mode_seal(safe_mode_rec_t *r) {
    r->magic = SAFE_MODE_MAGIC;
    r->check = safe_mode_sum(r);
}

bool safe_mode_valid(safe_mode_rec_t const *r) {
    return r->magic == SAFE_MODE_MAGIC && r->check == safe_mode_sum(r) && r->crashed[SAFE_MODE_ID_CAP - 1] == '\0';
}

void safe_mode_reset(safe_mode_rec_t *r) {
    uint8_t volatile *p = (uint8_t volatile *)r;
    for (size_t i = 0; i < sizeof(*r); i++) {
        p[i] = 0;
    }
    safe_mode_seal(r);
}

void safe_mode_ensure(safe_mode_rec_t *r) {
    if (!safe_mode_valid(r)) {
        safe_mode_reset(r);
    }
}

void safe_mode_note_panic(safe_mode_rec_t *r, char const *id, bool in_callback) {
    safe_mode_ensure(r);
    bool           ok  = safe_mode_id_ok(id);
    char volatile *out = r->crashed;
    for (size_t i = 0; i < SAFE_MODE_ID_CAP; i++) {
        out[i] = ok && i < SAFE_MODE_ID_CAP - 1 ? id[i] : '\0';
        if (ok && id[i] == '\0') {
            ok = false; // the rest stays zero
        }
    }
    r->flags = (r->flags & ~SAFE_MODE_F_UI_CB) | SAFE_MODE_F_PANIC | (in_callback ? SAFE_MODE_F_UI_CB : 0u);
    safe_mode_seal(r);
}
