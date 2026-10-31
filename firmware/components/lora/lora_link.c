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

bool lora_cfg_parse(char const *text, size_t len, lora_config_t *out, char *err, size_t errlen) {
    lora_config_t const defaults = LORA_CONFIG_DEFAULT;
    *out                         = defaults;
    if (errlen) {
        err[0] = '\0';
    }

    toml_result_t result = toml_parse(text, (int)len);
    if (!result.ok) {
        char why[sizeof(result.errmsg)];
        snprintf(why, sizeof(why), "%s", result.errmsg);
        toml_free(result);
        return fail(out, err, errlen, why);
    }

    lora_config_t c   = defaults;
    char const   *bad = NULL;
    double        num;
    bool          present;
    int64_t       v;

    if (!get_number(result.toptab, "frequency_mhz", &num, &present) || (present && !(num >= 862 && num <= 928))) {
        bad = "frequency_mhz must be 862-928 MHz";
    } else if (present) {
        c.freq_hz = (uint32_t)lround(num * 1e6);
    }
    if (!bad &&
        (!get_number(result.toptab, "bandwidth_khz", &num, &present) || (present && !(num > 0 && num <= 500)))) {
        bad = "bandwidth_khz must be a LoRa bandwidth up to 500 kHz";
    } else if (!bad && present) {
        c.bw_hz = (uint32_t)lround(num * 1e3);
    }

    v = c.sf;
    if (!bad && !get_int(result.toptab, "spreading_factor", 0, 255, &v)) {
        bad = "spreading_factor must be an integer";
    }
    c.sf = (uint8_t)v;
    v    = c.cr;
    if (!bad && !get_int(result.toptab, "coding_rate", 0, 255, &v)) {
        bad = "coding_rate must be an integer (5 = 4/5 ... 8 = 4/8)";
    }
    c.cr = (uint8_t)v;
    v    = c.sync_word;
    if (!bad && !get_int(result.toptab, "sync_word", 0, 255, &v)) {
        bad = "sync_word must be an integer 0-255";
    }
    c.sync_word = (uint8_t)v;
    v           = c.preamble;
    if (!bad && !get_int(result.toptab, "preamble", 0, 65535, &v)) {
        bad = "preamble must be an integer 0-65535";
    }
    c.preamble = (uint16_t)v;
    toml_free(result);

    if (bad) {
        return fail(out, err, errlen, bad);
    }
    if (!lora_config_valid(&c)) {
        return fail(
            out,
            err,
            errlen,
            "values out of range (862-928 MHz, LoRa bandwidth, SF 5-12, CR 5-8, preamble >= 6)"
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
    lora_config_t const *cfg,
    lora_status_t const *st,
    bool                 have_status,
    uint32_t             status_age_s,
    uint32_t             p4_dropped,
    uint32_t             p4_bad,
    char                *out,
    size_t               cap
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
                            "p4 dropped %u bad %u | "
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
            (unsigned)p4_dropped,
            (unsigned)p4_bad,
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
            "c6 no status yet | p4 dropped %u bad %u",
            (unsigned)p4_dropped,
            (unsigned)p4_bad
        );
    }
    used += n < 0 ? 0 : (size_t)n;
    return used < cap ? used : cap - 1;
}
