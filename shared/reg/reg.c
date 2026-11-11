// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "reg.h"

#include <stdio.h>

#include <string.h>

reg_entry_t const *reg_find_id(reg_entry_t const *tab, size_t n, uint16_t id) {
    for (size_t i = 0; i < n; i++) {
        if (tab[i].id == id) {
            return &tab[i];
        }
    }
    return NULL;
}

reg_entry_t const *reg_find_name(reg_entry_t const *tab, size_t n, char const *name) {
    for (size_t i = 0; name && i < n; i++) {
        if (strcmp(tab[i].name, name) == 0) {
            return &tab[i];
        }
    }
    return NULL;
}

bool reg_name_matches(char const *name, char const *query) {
    if (!query || !*query) {
        return true;
    }
    size_t qlen = strlen(query);
    if (strncmp(name, query, qlen) != 0) {
        return false;
    }
    return query[qlen - 1] == '.' || name[qlen] == '\0' || name[qlen] == '.';
}

static bool is_signed(uint8_t type) {
    return type == REG_T_I16 || type == REG_T_I32;
}

// 0..15 for a hex digit, 16 for anything else
static unsigned digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') {
        return (unsigned)((c | 0x20) - 'a' + 10);
    }
    return 16;
}

// Digits only, no sign: decimal without leading zeros ("0" itself is fine) or 0x hex. Above 0xFFFFFFFF is refused.
static bool parse_magnitude(char const *t, uint64_t *out) {
    unsigned base = 10;
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        base  = 16;
        t    += 2;
    } else if (t[0] == '0' && t[1] != '\0') {
        return false; // "010" would read as octal elsewhere: refuse rather than guess
    }
    uint64_t v = 0;
    for (char const *p = t; *p; p++) {
        unsigned d = digit_value(*p);
        if (d >= base || (v = v * base + d) > UINT32_MAX) {
            return false;
        }
    }
    *out = v;
    return *t != '\0';
}

// The whole text as one number that fits 32 bits (unsigned types: 0..UINT32_MAX, signed: INT32_MIN..INT32_MAX). Only
// signed types take a leading '-'; no '+', no whitespace.
static bool parse_number(char const *text, bool signed_type, reg_value_t *out) {
    bool     neg = signed_type && text[0] == '-';
    uint64_t mag;
    if (!parse_magnitude(text + neg, &mag)) {
        return false;
    }
    if (!signed_type) {
        out->u = (uint32_t)mag;
        return true;
    }
    if (mag > (neg ? (uint64_t)INT32_MAX + 1 : (uint64_t)INT32_MAX)) {
        return false;
    }
    out->i = neg ? (int32_t)(-(int64_t)mag) : (int32_t)mag;
    return true;
}

static bool parse_name(char const *const *names, int64_t max, char const *text, uint32_t *out) {
    for (int64_t i = 0; names && i <= max; i++) {
        if (names[i] && strcmp(names[i], text) == 0) {
            *out = (uint32_t)i;
            return true;
        }
    }
    return false;
}

bool reg_parse(reg_entry_t const *e, char const *text, reg_value_t *out) {
    static char const *const bool_names[] = {"off", "on"};
    static char const *const bool_alias[] = {"false", "true"};

    if (!text) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    switch (e->type) {
        case REG_T_STR:
            if (strlen(text) > REG_STR_MAX) {
                return false;
            }
            strcpy(out->s, text);
            return true;
        case REG_T_BOOL:
            if (parse_name(bool_names, 1, text, &out->u) || parse_name(bool_alias, 1, text, &out->u)) {
                return true;
            }
            break;
        case REG_T_ENUM:
            if (parse_name(e->enum_names, e->max, text, &out->u)) {
                return true;
            }
            break;
        default: break;
    }
    return parse_number(text, is_signed(e->type), out);
}

// The type's own range: what fits the type on either chip
static bool fits_type(uint8_t type, reg_value_t const *v) {
    switch (type) {
        case REG_T_U8: return v->u <= UINT8_MAX;
        case REG_T_I16: return v->i >= INT16_MIN && v->i <= INT16_MAX;
        case REG_T_BOOL: return v->u <= 1;
        case REG_T_U32:
        case REG_T_I32:
        case REG_T_ENUM: return true;
        case REG_T_STR: return memchr(v->s, '\0', sizeof(v->s)) != NULL;
        default: return false;
    }
}

reg_status_t reg_check(reg_entry_t const *e, reg_value_t const *v) {
    // min == max == 0: no range beyond the type's own. Not for ENUM: its range is always min..max (0..0 is one value)
    bool ranged = e->type == REG_T_ENUM || e->min != 0 || e->max != 0;
    if (e->type == REG_T_STR) {
        char const *nul = memchr(v->s, '\0', sizeof(v->s));
        int64_t     len = nul ? nul - v->s : -1;
        return len >= 0 && (!ranged || (len >= e->min && len <= e->max)) ? REG_OK : REG_RANGE;
    }
    if (!fits_type(e->type, v)) {
        return REG_RANGE;
    }
    int64_t x = is_signed(e->type) ? (int64_t)v->i : (int64_t)v->u;
    return !ranged || (x >= e->min && x <= e->max) ? REG_OK : REG_RANGE;
}

size_t reg_format(reg_entry_t const *e, reg_value_t const *v, char *buf, size_t cap) {
    char const *text = NULL;
    int         len;
    if (e->type == REG_T_BOOL && v->u <= 1) {
        text = v->u ? "on" : "off";
    } else if (e->type == REG_T_ENUM && e->enum_names && (int64_t)v->u <= e->max) {
        text = e->enum_names[v->u];
    } else if (e->type == REG_T_STR) {
        len = snprintf(buf, cap, "%.*s", REG_STR_MAX, v->s);
        return len >= 0 && (size_t)len < cap ? (size_t)len : 0;
    }
    if (text) {
        len = snprintf(buf, cap, "%s", text);
    } else if (is_signed(e->type)) {
        len = snprintf(buf, cap, "%ld", (long)v->i);
    } else {
        len = snprintf(buf, cap, "%lu", (unsigned long)v->u);
    }
    return len >= 0 && (size_t)len < cap ? (size_t)len : 0;
}

reg_status_t reg_get(reg_entry_t const *e, reg_value_t *out) {
    return reg_get_with(e, e->get_applied, out);
}

reg_status_t reg_get_with(reg_entry_t const *e, reg_get_fn get, reg_value_t *out) {
    memset(out, 0, sizeof(*out));
    if (!get) {
        return REG_FAILED;
    }
    // A getter value that does not fit the key's type (a U8 of 300, an unterminated STR) is a driver fault, not a value
    reg_status_t st = get(e, out);
    // An ENUM code past its name table is the same fault: no consumer may index the names with it
    if (st != REG_OK || !fits_type(e->type, out) || (e->type == REG_T_ENUM && reg_check(e, out) != REG_OK)) {
        memset(out, 0, sizeof(*out));
        return REG_FAILED;
    }
    return REG_OK;
}

static reg_status_t try_set(reg_entry_t const *e, reg_value_t const *v) {
    if (e->flags & REG_F_TX) {
        return REG_GUARDED;
    }
    if ((e->flags & REG_F_RO) || !e->set) {
        return REG_RO;
    }
    if (reg_check(e, v) != REG_OK) {
        return REG_RANGE;
    }
    return e->set(e, v) == REG_OK ? REG_OK : REG_FAILED;
}

reg_status_t reg_set(reg_entry_t const *e, reg_value_t const *v, reg_value_t *applied) {
    reg_status_t st   = try_set(e, v);
    reg_status_t read = reg_get(e, applied);
    // No readable applied value: FAILED whatever the set said, so no answer ever pairs GUARDED or RANGE with a 0
    return read != REG_OK ? REG_FAILED : st;
}

reg_status_t reg_serve(reg_entry_t const *tab, size_t n, uint16_t id, uint32_t const *set, reg_reply_t *out) {
    reg_entry_t const *e = reg_find_id(tab, n, id);
    out->type            = 0;
    out->value           = 0;
    if (!e) {
        return REG_UNKNOWN;
    }
    out->type = e->type;
    if (e->type == REG_T_STR) {
        return REG_FAILED;
    }
    reg_value_t  got;
    reg_status_t st = set ? reg_set(e, &(reg_value_t){.u = *set}, &got) : reg_get(e, &got);
    out->value      = got.u;
    return st;
}

bool reg_type_known(uint8_t type) {
    return type >= REG_T_U8 && type <= REG_T_STR;
}

// Lower case dotted: [a-z0-9_] segments joined by single dots, at least two segments, at most REG_NAME_MAX long
static bool name_ok(char const *name) {
    if (!name) {
        return false;
    }
    size_t len  = strlen(name);
    bool   dot  = false;
    char   prev = '.';
    for (char const *p = name; *p; prev = *p++) {
        bool word = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!word && (*p != '.' || prev == '.')) {
            return false;
        }
        dot |= *p == '.';
    }
    return dot && len <= REG_NAME_MAX && prev != '.';
}

static bool entry_ok(reg_entry_t const *e) {
    if (!name_ok(e->name) || !reg_type_known(e->type) || !e->get_applied || e->min > e->max) {
        return false;
    }
    if ((e->flags & REG_F_TX) && e->set && !(e->flags & REG_F_RO)) {
        return false; // a TX key is never set through the registry: a setter would only hide that
    }
    if (e->type == REG_T_STR) {
        return e->max <= REG_STR_MAX;
    }
    if (e->type != REG_T_ENUM) {
        return true;
    }
    if (e->min != 0 || !e->enum_names || e->max > UINT8_MAX) {
        return false;
    }
    for (int64_t i = 0; i <= e->max; i++) {
        if (!e->enum_names[i]) {
            return false;
        }
    }
    return true;
}

size_t reg_table_check(reg_entry_t const *tab, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!entry_ok(&tab[i])) {
            return i;
        }
        for (size_t j = 0; j < i; j++) {
            if (tab[j].id == tab[i].id || strcmp(tab[j].name, tab[i].name) == 0) {
                return i;
            }
        }
    }
    return n;
}
