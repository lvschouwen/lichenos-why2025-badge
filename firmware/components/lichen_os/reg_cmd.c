#include "reg_cmd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

char const *reg_note_text(rv_note_t note) {
    switch (note) {
        case RV_NONE: return "";
        case RV_STALE: return "stale";
        case RV_NO_ANSWER: return "no answer from the C6";
        case RV_OLD_C6: return "not supported by this C6";
        case RV_NO_DOMAIN: return "not supported by this C6 (domain)";
        case RV_UNKNOWN_ID: return "not supported by this C6 (unknown id)";
        case RV_NO_LINK: return "no link to the C6";
        case RV_FAILED: return "failed";
        case RV_WAITING: return "waiting for the C6";
        case RV_NOT_APPLIED: return "not applied yet";
    }
    return "?";
}

// The SX126x LoRa bandwidths, Hz (rounded): every bw maps to the nearest of them
static uint32_t const sx126x_bw[] = {7810, 10420, 15630, 20830, 31250, 41670, 62500, 125000, 250000, 500000};

static unsigned bw_code(uint32_t hz) {
    unsigned best = 0;
    uint32_t dist = UINT32_MAX;
    for (unsigned i = 0; i < sizeof(sx126x_bw) / sizeof(sx126x_bw[0]); i++) {
        uint32_t d = hz > sx126x_bw[i] ? hz - sx126x_bw[i] : sx126x_bw[i] - hz;
        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    return best;
}

static uint32_t bl_expected(uint32_t pct) {
    return !pct ? 0 : pct < 10 ? 10 : pct > 80 ? 80 : pct;
}

reg_match_t reg_applied_matches(uint16_t id, reg_value_t const *requested, reg_value_t const *applied) {
    uint32_t r = requested->u, a = applied->u;
    if (r == a) {
        return RM_EQUAL;
    }
    bool ok;
    switch (id) {
        case REG_ID_LORA_FREQ: ok = (r > a ? r - a : a - r) <= REG_FREQ_TOLERANCE_HZ; break;
        case REG_ID_LORA_BW: ok = bw_code(r) == bw_code(a); break;
        case REG_ID_BL_DISPLAY:
        case REG_ID_BL_KEYBOARD: ok = bl_expected(r) == a; break;
        default: ok = false; break;
    }
    return ok ? RM_TOLERATED : RM_DIFFERS;
}

static size_t done(int n, size_t cap) {
    if (n < 0) {
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

#define ROW_FMT "%-15s %-12s %-12s %-12s %-5s %-8s %s"

size_t reg_cmd_header(char *buf, size_t cap) {
    return cap ? done(snprintf(buf, cap, ROW_FMT, "key", "saved", "requested", "applied", "unit", "age", ""), cap) : 0;
}

static void value_text(reg_entry_t const *e, bool has, reg_value_t const *v, char const *none, char *out, size_t cap) {
    if (!has || !reg_format(e, v, out, cap)) {
        snprintf(out, cap, "%s", none);
    }
}

static void age_text(reg_view_t const *v, char *out, size_t cap) {
    if (!v->has_age) {
        snprintf(out, cap, "-");
    } else if (v->age_ms < 100000) {
        snprintf(out, cap, "%u.%u s", (unsigned)(v->age_ms / 1000), (unsigned)(v->age_ms % 1000 / 100));
    } else {
        snprintf(out, cap, "%u s", (unsigned)(v->age_ms / 1000));
    }
}

size_t reg_cmd_row(reg_entry_t const *e, reg_view_t const *v, char *buf, size_t cap) {
    char saved[REG_STR_MAX + 8], req[REG_STR_MAX + 8], raw[REG_STR_MAX + 8], applied[REG_STR_MAX + 12], age[16];
    if (!cap) {
        return 0;
    }
    value_text(e, v->has_saved, &v->saved, "-", saved, sizeof(saved));
    value_text(e, v->has_requested, &v->requested, "-", req, sizeof(req));
    value_text(e, v->has_applied, &v->applied, "?", raw, sizeof(raw));
    bool near = v->has_applied && v->has_requested &&
                reg_applied_matches(e->id, &v->requested, &v->applied) == RM_TOLERATED;
    // "≈": the chip's own step or clamp, not a mismatch
    snprintf(applied, sizeof(applied), near ? "\xe2\x89\x88%s" : "%s", raw);
    age_text(v, age, sizeof(age));
    // Trailing blanks of the unit/age columns are kept short: the note is the last column
    int n = snprintf(buf, cap, ROW_FMT, e->name, saved, req, applied, e->unit ? e->unit : "", age,
                     reg_note_text((rv_note_t)v->note));
    size_t len = done(n, cap);
    while (len && buf[len - 1] == ' ') {
        buf[--len] = '\0';
    }
    return len;
}

// "<key> <value>" split in place into the copies; false when either is missing
static bool split_args(char const *args, char *key, size_t kcap, char const **value) {
    char const *sp = strchr(args, ' ');
    if (!sp || sp == args || (size_t)(sp - args) >= kcap || !sp[1]) {
        return false;
    }
    memcpy(key, args, (size_t)(sp - args));
    key[sp - args] = '\0';
    *value         = sp + 1;
    return true;
}

// reg_set's own order (TX -> GUARDED, RO or no setter -> RO, range -> RANGE), checked here first so a refused set
// still reports its reason when the applied value cannot be read (reg_set would say FAILED then) and never calls a
// setter: a TX key reaches nothing that sends (#307 rule, pinned by the test)
static reg_cmd_result_t refusal(reg_entry_t const *e, reg_value_t const *v) {
    if (e->flags & REG_F_TX) {
        return RC_GUARDED;
    }
    if ((e->flags & REG_F_RO) || !e->set) {
        return RC_RO;
    }
    return reg_check(e, v) == REG_OK ? RC_OK : RC_RANGE;
}

reg_cmd_result_t reg_cmd_set(reg_find_name_fn find, char const *args, reg_confirmable_fn confirmable,
                             reg_entry_t const **e, reg_value_t *applied, bool *have_applied) {
    char        key[REG_NAME_MAX + 1];
    char const *text;
    *e            = NULL;
    *have_applied = false;
    if (!split_args(args, key, sizeof(key), &text)) {
        return RC_USAGE;
    }
    *e = find(key);
    if (!*e) {
        return RC_UNKNOWN;
    }
    reg_value_t v;
    if (!reg_parse(*e, text, &v)) {
        return RC_BAD_VALUE;
    }
    reg_cmd_result_t r = refusal(*e, &v);
    if (r != RC_OK) {
        *have_applied = reg_get(*e, applied) == REG_OK;
        return r;
    }
    reg_status_t st = reg_set(*e, &v, applied);
    *have_applied   = st == REG_OK;
    if (st != REG_OK && confirmable && !confirmable(*e)) {
        *applied      = v; // the level asked for, said as such in the reply
        *have_applied = true;
        return RC_UNCONFIRMED;
    }
    return st == REG_OK ? RC_OK : RC_FAILED;
}

static size_t applied_suffix(reg_entry_t const *e, reg_value_t const *applied, bool have, char *buf, size_t cap) {
    char v[REG_STR_MAX + 8];
    if (!have || !reg_format(e, applied, v, sizeof(v))) {
        return done(snprintf(buf, cap, "applied ?"), cap);
    }
    return done(snprintf(buf, cap, "applied %s%s%s", v, e->unit ? " " : "", e->unit ? e->unit : ""), cap);
}

size_t reg_cmd_set_reply(reg_cmd_result_t r, reg_entry_t const *e, reg_value_t const *applied, bool have_applied,
                         char const *args, char *buf, size_t cap) {
    char tail[48];
    if (!cap) {
        return 0;
    }
    if (r == RC_USAGE) {
        return done(snprintf(buf, cap, "set <key> <value> (fw get lists the keys)"), cap);
    }
    if (r == RC_UNKNOWN || !e) {
        return done(snprintf(buf, cap, "ERR unknown key: %s", args), cap);
    }
    if (r == RC_BAD_VALUE) {
        return done(snprintf(buf, cap, "ERR bad value for %s", e->name), cap);
    }
    applied_suffix(e, applied, have_applied, tail, sizeof(tail));
    switch (r) {
        case RC_OK: return done(snprintf(buf, cap, "%s = %s", e->name, tail + strlen("applied ")), cap);
        case RC_UNCONFIRMED:
            return done(snprintf(buf, cap, "%s = %s (sent over UART, not confirmed by this C6)", e->name,
                                 tail + strlen("applied ")),
                        cap);
        case RC_GUARDED:
            return done(snprintf(buf, cap, "ERR guarded: %s changes what the badge transmits, use its own command (%s)",
                                 e->name, tail),
                        cap);
        case RC_RO: return done(snprintf(buf, cap, "ERR read-only: %s (%s)", e->name, tail), cap);
        case RC_RANGE:
            return done(snprintf(buf, cap, "ERR range: %s takes %lld..%lld (%s)", e->name, (long long)e->min,
                                 (long long)e->max, tail),
                        cap);
        default: break;
    }
    return done(snprintf(buf, cap, "ERR failed: %s set, applied not read back", e->name), cap);
}

size_t reg_cached_text(reg_entry_t const *e, reg_view_t const *v, char *buf, size_t cap) {
    char val[REG_STR_MAX + 1], age[16];
    if (!cap) {
        return 0;
    }
    if (!v->has_applied || !reg_format(e, &v->applied, val, sizeof(val))) {
        rv_note_t note = (rv_note_t)v->note;
        return done(snprintf(buf, cap, "? (%s)", note == RV_NONE ? "not read yet" : reg_note_text(note)), cap);
    }
    if (!v->has_age) {
        return done(snprintf(buf, cap, "%s", val), cap);
    }
    age_text(v, age, sizeof(age));
    return done(snprintf(buf, cap, "%s (%s%s ago)", val, v->note == RV_STALE ? "stale, " : "", age), cap);
}

static uint32_t max_age(reg_view_t const v[REG_RADIO_KEYS]) {
    uint32_t age = 0;
    for (int i = 0; i < REG_RADIO_KEYS; i++) {
        age = v[i].age_ms > age ? v[i].age_ms : age;
    }
    return age;
}

size_t reg_radio_text(reg_view_t const v[REG_RADIO_KEYS], reg_entry_t const *state, char *buf, size_t cap) {
    if (!cap) {
        return 0;
    }
    for (int i = 0; i < REG_RADIO_KEYS; i++) {
        if (!v[i].has_applied) {
            rv_note_t note = (rv_note_t)v[i].note;
            return done(snprintf(buf, cap, "? (%s)", note == RV_NONE ? "no answer from the C6" : reg_note_text(note)),
                        cap);
        }
    }
    char st[REG_STR_MAX + 1];
    if (!reg_format(state, &v[7].applied, st, sizeof(st))) {
        snprintf(st, sizeof(st), "?");
    }
    bool     stale = false;
    uint32_t age   = max_age(v);
    for (int i = 0; i < REG_RADIO_KEYS; i++) {
        stale = stale || v[i].note == RV_STALE;
    }
    int n = snprintf(buf, cap, "%.3f MHz %g kHz SF%u CR%u sync 0x%02X pre %u %d dBm %s | %u.%u s ago%s",
                     v[0].applied.u / 1e6, v[1].applied.u / 1e3, (unsigned)v[2].applied.u, (unsigned)v[3].applied.u,
                     (unsigned)v[4].applied.u, (unsigned)v[5].applied.u, (int)v[6].applied.i, st,
                     (unsigned)(age / 1000), (unsigned)(age % 1000 / 100), stale ? " (stale)" : "");
    return done(n, cap);
}
