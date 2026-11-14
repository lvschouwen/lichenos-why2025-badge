#include "lora_link.h"

#include "thirdparty/tomlc17.h"

#include <stdio.h>

#include <math.h>
#include <string.h>

// A number from the file: frequency and bandwidth may be written as integers or floats
static bool get_number(toml_datum_t tab, char const *key, double *out, bool *present) {
    toml_datum_t d = toml_get(tab, key);
    *present       = d.type != TOML_UNKNOWN;
    if (d.type == TOML_INT64) {
        *out = (double)d.u.int64;
    } else if (d.type == TOML_FP64) {
        *out = d.u.fp64;
    } else {
        return !*present;
    }
    return true;
}

// An integer key within [min, max]; a missing key leaves *out alone
static bool get_int(toml_datum_t tab, char const *key, int64_t min, int64_t max, int64_t *out) {
    toml_datum_t d = toml_get(tab, key);
    if (d.type == TOML_UNKNOWN) {
        return true;
    }
    if (d.type != TOML_INT64 || d.u.int64 < min || d.u.int64 > max) {
        return false;
    }
    *out = d.u.int64;
    return true;
}

static bool fail(lora_config_t *out, char *err, size_t errlen, char const *why) {
    lora_config_t const defaults = LORA_CONFIG_DEFAULT;
    *out                         = defaults;
    snprintf(err, errlen, "%s", why);
    return false;
}

// frequency_mhz and bandwidth_khz, integers or floats: the reason the first bad one is refused, NULL when both are fine
static char const *read_floats(toml_datum_t tab, lora_config_t *c) {
    double num;
    bool   present;
    if (!get_number(tab, "frequency_mhz", &num, &present) || (present && !(num >= 862 && num <= 928))) {
        return "frequency_mhz must be 862-928 MHz";
    }
    if (present) {
        c->freq_hz = (uint32_t)lround(num * 1e6);
    }
    if (!get_number(tab, "bandwidth_khz", &num, &present) || (present && !(num > 0 && num <= 500))) {
        return "bandwidth_khz must be a LoRa bandwidth up to 500 kHz";
    }
    if (present) {
        c->bw_hz = (uint32_t)lround(num * 1e3);
    }
    return NULL;
}

// The integer keys, in the file's order of checks; ranges here are only what the field holds, lora_config_valid has
// the radio's
static char const *read_ints(toml_datum_t tab, lora_config_t *c) {
    int64_t v = c->sf;
    if (!get_int(tab, "spreading_factor", 0, 255, &v)) {
        return "spreading_factor must be an integer";
    }
    c->sf = (uint8_t)v;
    v     = c->cr;
    if (!get_int(tab, "coding_rate", 0, 255, &v)) {
        return "coding_rate must be an integer (5 = 4/5 ... 8 = 4/8)";
    }
    c->cr = (uint8_t)v;
    v     = c->sync_word;
    if (!get_int(tab, "sync_word", 0, 255, &v)) {
        return "sync_word must be an integer 0-255";
    }
    c->sync_word = (uint8_t)v;
    v            = c->preamble;
    if (!get_int(tab, "preamble", 0, 65535, &v)) {
        return "preamble must be an integer 0-65535";
    }
    c->preamble = (uint16_t)v;
    return NULL;
}

// Not inlined, so the keys' lookups do not add to the frame that already holds the parse result (#212)
__attribute__((noinline)) static char const *read_keys(toml_datum_t tab, lora_config_t *c) {
    char const *bad = read_floats(tab, c);
    return bad ? bad : read_ints(tab, c);
}

bool lora_cfg_parse(char const *text, size_t len, lora_config_t *out, char *err, size_t errlen) {
    lora_config_t const defaults = LORA_CONFIG_DEFAULT;
    *out                         = defaults;
    if (errlen) {
        err[0] = '\0';
    }

    toml_result_t result = toml_parse(text, (int)len);
    lora_config_t c      = defaults;
    char const   *bad    = result.ok ? read_keys(result.toptab, &c) : NULL;
    bool const    parsed = result.ok;
    if (!parsed) {
        fail(out, err, errlen, result.errmsg); // copied out before the free below
    }
    toml_free(result);

    if (!parsed) {
        return false;
    }
    if (bad) {
        return fail(out, err, errlen, bad);
    }
    if (!lora_config_valid(&c)) {
        return fail(
            out,
            err,
            errlen,
            "values out of range (862-928 MHz, LoRa bandwidth, SF 5-12, CR 5-8, preamble 6-64)"
        );
    }
    *out = c;
    return true;
}

void lora_sup_init(lora_sup_t *s, uint32_t now_ms) {
    memset(s, 0, sizeof(*s));
    s->now_ms       = now_ms;
    s->last_poll_ms = now_ms;
}

unsigned lora_sup_tick(lora_sup_t *s, uint32_t now_ms) {
    s->now_ms = now_ms;
    if (!s->configured) {
        s->configured   = true;
        s->last_poll_ms = now_ms;
        return LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START;
    }
    if (now_ms - s->last_poll_ms < LORA_POLL_MS) {
        return 0;
    }

    s->last_poll_ms = now_ms;
    unsigned act    = LORA_ACT_STATUS_REQ;
    if (++s->unanswered_polls >= LORA_SILENT_POLLS) {
        // The C6 may have restarted without peer data; configure it again along with this poll
        s->unanswered_polls  = 0;
        act                 |= LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START;
    }
    return act;
}

unsigned lora_sup_on_status(lora_sup_t *s, lora_status_t const *st, uint32_t now_ms) {
    s->now_ms           = now_ms;
    s->last_status_ms   = now_ms;
    s->unanswered_polls = 0;
    if (st->state != LORA_STATE_ERROR) {
        s->error_reports = 0;
    }
    if (st->state == LORA_STATE_UNCONFIGURED || st->state == LORA_STATE_IDLE) {
        s->configured = true;
        return LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START;
    }
    // A failed radio is tried again now and then, not on every report: one SPI hiccup must not end reception
    if (st->state == LORA_STATE_ERROR && ++s->error_reports >= LORA_ERROR_RETRY_REPORTS) {
        s->error_reports = 0;
        return LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START;
    }
    return 0;
}

size_t lora_status_format(
    lora_config_t const   *cfg,
    lora_status_t const   *st,
    bool                   have_status,
    uint32_t               status_age_s,
    lora_p4_stats_t const *p4,
    char                  *out,
    size_t                 cap
) {
    static char const *const states[] = {"unconfigured", "idle", "rx", "error", "tx"};

    if (!cap) {
        return 0;
    }
    int n = snprintf(
        out,
        cap,
        "%.3f MHz %g kHz SF%u CR%u sync 0x%02X pre %u | ",
        cfg->freq_hz / 1e6,
        cfg->bw_hz / 1e3,
        cfg->sf,
        cfg->cr,
        cfg->sync_word,
        cfg->preamble
    );
    size_t used = n < 0 ? 0 : (size_t)n;
    if (used >= cap) {
        return cap - 1;
    }

    if (have_status) {
        char const *state = st->state < sizeof(states) / sizeof(states[0]) ? states[st->state] : "?";
        n                 = snprintf(
            out + used,
            cap - used,
            "c6 %s rssi %.1f | packets %u crc %u hdr %u dropped %u ignored %u | c6 up %u s heap %u/%u KiB stack %u B | "
                            "p4 dropped %u bad %u reg in flight %u timeouts %u | "
                            "status %u s ago",
            state,
            st->rssi_x10 / 10.0,
            (unsigned)st->packets,
            (unsigned)st->crc_errors,
            (unsigned)st->header_errors,
            (unsigned)st->dropped,
            (unsigned)st->ignored,
            (unsigned)st->uptime_s,
            (unsigned)(st->heap_free / 1024),
            (unsigned)(st->heap_min / 1024),
            (unsigned)st->stack_free,
            (unsigned)p4->dropped,
            (unsigned)p4->bad,
            (unsigned)p4->reg_in_flight,
            (unsigned)p4->reg_timeouts,
            (unsigned)status_age_s
        );
        if (n >= 0 && st->last_radio_err && (size_t)n < cap - used) {
            used += (size_t)n;
            n     = snprintf(out + used, cap - used, " | radio error %d", st->last_radio_err);
        }
    } else {
        n = snprintf(
            out + used,
            cap - used,
            "c6 no status yet | p4 dropped %u bad %u reg in flight %u timeouts %u",
            (unsigned)p4->dropped,
            (unsigned)p4->bad,
            (unsigned)p4->reg_in_flight,
            (unsigned)p4->reg_timeouts
        );
    }
    used += n < 0 ? 0 : (size_t)n;
    return used < cap ? used : cap - 1;
}

void lora_c6_link_init(lora_c6_link_t *l) {
    memset(l, 0, sizeof(*l));
}

void lora_c6_link_lost(lora_c6_link_t *l) {
    l->lost         = true;
    l->back_pending = false;
    l->up_pending   = false;
    l->down_pending = true;
}

void lora_c6_link_back(lora_c6_link_t *l) {
    if (l->lost) {
        l->back_pending = true;
    }
}

unsigned lora_c6_link_take(lora_c6_link_t *l) {
    unsigned act = 0;
    if (l->down_pending) {
        l->down_pending = false;
        act |= LORA_C6_TELL_DOWN;
    }
    if (l->back_pending) {
        l->back_pending = false;
        l->lost         = false;
        l->up_pending   = true;
        act |= LORA_C6_REINIT;
    }
    return act;
}

unsigned lora_c6_link_heard(lora_c6_link_t *l) {
    if (l->up_pending && !l->lost) {
        l->up_pending = false;
        return LORA_C6_TELL_UP;
    }
    return 0;
}
