// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "reg.h"

#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
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

// The whole text as one number that fits 32 bits (unsigned types: 0..UINT32_MAX, signed: INT32_MIN..INT32_MAX)
static bool parse_number(char const *text, bool signed_type, reg_value_t *out) {
    char *end = NULL;
    errno     = 0;
    if (!*text || (!signed_type && strchr(text, '-'))) {
        return false;
    }
    long long v = strtoll(text, &end, 0);
    if (errno || *end || (signed_type ? v < INT32_MIN || v > INT32_MAX : v < 0 || v > (long long)UINT32_MAX)) {
        return false;
    }
    if (signed_type) {
        out->i = (int32_t)v;
    } else {
        out->u = (uint32_t)v;
    }
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
        default: return false;
    }
}

reg_status_t reg_check(reg_entry_t const *e, reg_value_t const *v) {
    // min == max == 0: no range beyond the type's own
    bool ranged = e->min != 0 || e->max != 0;
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
    memset(out, 0, sizeof(*out));
    if (!e->get_applied) {
        return REG_FAILED;
    }
    reg_status_t st = e->get_applied(e, out);
    if (st != REG_OK) {
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
    return st == REG_OK && read != REG_OK ? REG_FAILED : st;
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
