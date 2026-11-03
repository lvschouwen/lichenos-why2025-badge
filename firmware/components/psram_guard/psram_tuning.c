#include "psram_tuning.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool psram_tuning_dl_args(char const *text, unsigned *idx) {
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (!isdigit((unsigned char)*text)) {
        return false;
    }
    char         *end;
    unsigned long v = strtoul(text, &end, 10);
    if (end - text > 2 || !isspace((unsigned char)*end)) { // idx 0..30: at most two digits, then a space
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (strncmp(end, "yes", 3) != 0) {
        return false;
    }
    for (end += 3; isspace((unsigned char)*end); end++) {
    }
    if (*end) {
        return false;
    }
    *idx = (unsigned)v;
    return true;
}

bool psram_tuning_dl_in_window(psram_tuning_t const *t, unsigned idx) {
    return !psram_tuning_fell_back(t) && idx >= psram_tuning_dl_start(t) && idx <= t->dl_end;
}

uint32_t psram_carrier_pack(uint16_t value) {
    return ((uint32_t)PSRAM_CARRIER_MAGIC << 16) | value;
}

bool psram_carrier_unpack(uint32_t word, uint16_t *value) {
    if ((word >> 16) != PSRAM_CARRIER_MAGIC) {
        return false;
    }
    *value = (uint16_t)(word & 0xFFFF);
    return true;
}

psram_oneshot_verdict_t psram_oneshot_verdict(psram_oneshot_t const *rec, psram_tuning_t const *t) {
    if (rec->magic != PSRAM_ONESHOT_MAGIC || rec->idx >= PSRAM_DELAYLINES) {
        return PSRAM_ONESHOT_BAD_RECORD;
    }
    if (psram_tuning_fell_back(t)) {
        return PSRAM_ONESHOT_FELL_BACK;
    }
    return psram_tuning_dl_in_window(t, rec->idx) ? PSRAM_ONESHOT_APPLY : PSRAM_ONESHOT_OUTSIDE;
}

char const *psram_oneshot_verdict_text(psram_oneshot_verdict_t v) {
    switch (v) {
        case PSRAM_ONESHOT_APPLY: return "applied";
        case PSRAM_ONESHOT_BAD_RECORD: return "ignored: bad record";
        case PSRAM_ONESHOT_FELL_BACK: return "ignored: the boot tuning fell back";
        case PSRAM_ONESHOT_OUTSIDE: return "ignored: outside this boot's tuning window";
    }
    return "ignored";
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
