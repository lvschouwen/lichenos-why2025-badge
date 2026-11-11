#include "reset_probe_logic.h"

#include <stdio.h>

#define PROBE_MAGIC 0x52505231u // "RPR1"

static uint32_t probe_check(uint32_t magic, uint32_t count) {
    uint32_t h = magic ^ 0xA5C3F00Fu;
    h          = (h << 9 | h >> 23) ^ count;
    return h * 2246822519u;
}

reset_probe_word_t reset_probe_make(uint32_t count) {
    return (reset_probe_word_t){.magic = PROBE_MAGIC, .count = count, .check = probe_check(PROBE_MAGIC, count)};
}

bool reset_probe_ok(reset_probe_word_t const *w) {
    return w->magic == PROBE_MAGIC && w->check == probe_check(w->magic, w->count);
}

reset_probe_word_t reset_probe_next(reset_probe_word_t const *w) {
    return reset_probe_make(reset_probe_ok(w) ? w->count + 1 : 0);
}

char const *reset_probe_line(char *out, size_t cap, char const *reason, unsigned rom, bool const ok[RESET_PROBE_STORES],
                             uint32_t const count[RESET_PROBE_STORES]) {
    if (cap == 0) {
        return out;
    }
    snprintf(out, cap, "reset %s (rom 0x%x), rtc %s, lp_store %s, psram %s (survived %u/%u/%u)", reason ? reason : "?",
             rom, ok[0] ? "ok" : "lost", ok[1] ? "ok" : "lost", ok[2] ? "ok" : "lost", (unsigned)count[0],
             (unsigned)count[1], (unsigned)count[2]);
    return out;
}
