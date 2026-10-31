#include "psram_tuning.h"

#include <stdio.h>

#define RETRY_GAP_S 600

void psram_tuning_note_phase(psram_tuning_t *t, uint32_t len, uint32_t end) {
    t->phase_seen = true;
    t->phase_len  = (uint8_t)len;
    t->phase_id   = len == 0 ? 0 : (uint8_t)(end - len + 1);
}

void psram_tuning_note_delayline(psram_tuning_t *t, uint32_t len, uint32_t end) {
    t->dl_seen = true;
    t->dl_len  = (uint8_t)len;
    t->dl_end  = (uint8_t)end;
    t->dl_idx  = len <= 1 ? 0 : (uint8_t)(end - len / 2);
}

void psram_tuning_delays(uint8_t idx, uint8_t *data, uint8_t *dqs) {
    *data = idx > PSRAM_IDX_ZERO ? (uint8_t)(idx - PSRAM_IDX_ZERO) : 0;
    *dqs  = idx < PSRAM_IDX_ZERO ? (uint8_t)(PSRAM_IDX_ZERO - idx) : 0;
}

unsigned psram_tuning_phase_centi(uint8_t phase_id) {
    static unsigned const centi[PSRAM_PHASES] = {6750, 7875, 9000, 10125};
    return centi[phase_id % PSRAM_PHASES];
}

bool psram_tuning_fell_back(psram_tuning_t const *t) {
    return !t->phase_seen || !t->dl_seen || t->phase_len == 0 || t->dl_len <= 1;
}

uint8_t psram_tuning_dl_start(psram_tuning_t const *t) {
    return t->dl_len ? (uint8_t)(t->dl_end - t->dl_len + 1) : 0;
}

size_t psram_tuning_format(char *out, size_t cap, psram_tuning_t const *t) {
    if (!t->phase_seen || !t->dl_seen) {
        return (size_t)snprintf(out, cap, "tuning not captured");
    }
    unsigned c = psram_tuning_phase_centi(t->phase_id);
    uint8_t  data, dqs;
    psram_tuning_delays(t->dl_idx, &data, &dqs);
    return (size_t)snprintf(out, cap, "phase %u.%02u (run %u/%d), delayline idx %u (data %u, dqs %u), window %u..%u len %u%s",
                            c / 100, c % 100, t->phase_len, PSRAM_PHASES, t->dl_idx, data, dqs, psram_tuning_dl_start(t),
                            t->dl_end, t->dl_len, psram_tuning_fell_back(t) ? " FELL BACK" : "");
}

bool psram_boot_verdict(psram_tuning_t const *t, size_t flipped_bytes, char *why, size_t cap) {
    bool fell  = psram_tuning_fell_back(t);
    unsigned const min = PSRAM_WINDOW_MIN; // a variable, so 0 (off) does not trip -Wtype-limits
    bool short_ = min > 0 && t->dl_len < min;
    bool bad   = fell || short_ || flipped_bytes > 0;
    if (bad) {
        snprintf(why, cap, "psram: ph%u dl%u w%u%s%s bad=%u", psram_tuning_phase_centi(t->phase_id) / 100, t->dl_idx, t->dl_len,
                 fell ? " fallback" : "", short_ ? " short" : "", (unsigned)flipped_bytes);
    } else if (cap) {
        why[0] = 0;
    }
    return bad;
}

bool psram_retry_decide(psram_retry_t *st, bool bad, uint32_t now_s) {
    if (!bad) {
        *st = (psram_retry_t){0};
        return false;
    }
    bool allowed = st->retries == 0 || (now_s != 0 && st->last_s != 0 && now_s - st->last_s >= RETRY_GAP_S);
    if (!allowed) {
        return false;
    }
    st->retries++;
    st->last_s = now_s;
    return true;
}
