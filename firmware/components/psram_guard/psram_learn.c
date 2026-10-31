#include "psram_learn.h"

void psram_learn_reset(psram_learn_t *r, unsigned speed_mhz) {
    *r = (psram_learn_t){.version = PSRAM_LEARN_VERSION, .speed_mhz = (uint16_t)speed_mhz, .lo = UINT32_MAX, .hi = 0};
}

bool psram_learn_empty(psram_learn_t const *r) {
    return r->lo > r->hi;
}

static bool in_psram(uintptr_t a) {
    return a >= PSRAM_ADDR_MIN && a < PSRAM_ADDR_END;
}

bool psram_learn_usable(psram_learn_t const *r, size_t size, unsigned speed_mhz) {
    if (size != sizeof(*r) || r->version != PSRAM_LEARN_VERSION || r->speed_mhz != speed_mhz) {
        return false;
    }
    return psram_learn_empty(r) || (in_psram(r->lo) && in_psram(r->hi) && r->hi - r->lo < PSRAM_LEARN_MAX_SPAN);
}

psram_learn_result_t psram_learn_note(psram_learn_t *r, uintptr_t lo, uintptr_t hi) {
    if (lo > hi || !in_psram(lo) || !in_psram(hi)) {
        return PSRAM_LEARN_OUTLIER;
    }
    uintptr_t nlo = psram_learn_empty(r) || lo < r->lo ? lo : r->lo;
    uintptr_t nhi = psram_learn_empty(r) || hi > r->hi ? hi : r->hi;
    if (nhi - nlo >= PSRAM_LEARN_MAX_SPAN) {
        return PSRAM_LEARN_OUTLIER;
    }
    if (!psram_learn_empty(r) && nlo == r->lo && nhi == r->hi) {
        return PSRAM_LEARN_SAME;
    }
    r->lo = (uint32_t)nlo;
    r->hi = (uint32_t)nhi;
    r->writes++;
    return PSRAM_LEARN_GREW;
}
