#include "settings_backup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_FW   "badge_fw"
#define NS_WX   "a.weather"

// Ranges follow the code that reads each key: idle_policy.h, saver_station.h, backlight_level.h, haptics_pattern.h,
// weather_series.h; the apps' own keys come with their registration (lichen_app_t.backup_keys). Defaults are what that code uses for a missing key.
static sb_key_t const keys[] = {
    {NS_FW, "home_layout", "Home layout", SB_U8, 0, 1, 0, NULL},
    {NS_FW, "tz", "Time zone", SB_STR, 1, 63, 0, "CET-1CEST,M3.5.0,M10.5.0/3"},
    {NS_FW, "idle_dim", "Dim after", SB_U8, 0, 255, 6, NULL},
    {NS_FW, "idle_off", "Screen off after", SB_U8, 0, 255, 30, NULL},
    {NS_FW, "saver", "Clock screensaver", SB_U8, 0, 1, 1, NULL},
    {NS_FW, "saver_style", "Screensaver style", SB_U8, 0, 1, 0, NULL},
    {NS_FW, "lock", "Pocket lock", SB_U8, 0, 1, 0, NULL},
    {NS_FW, "bl_disp", "Screen brightness", SB_U8, 5, 100, 30, NULL},
    {NS_FW, "bl_kbd", "Keyboard brightness", SB_U8, 0, 100, 10, NULL},
    {NS_FW, "kbd_pulse", "Message light", SB_U8, 0, 2, 1, NULL},
    {NS_FW, "haptics", "Vibration", SB_U8, 0, 1, 1, NULL},
    {NS_FW, "haptic_str", "Vibration strength", SB_U8, 0, 2, 2, NULL},
    {NS_FW, "status_led", "Status LED", SB_U8, 0, 1, 1, NULL},
    {NS_FW, "notify_dnd", "Do not disturb", SB_U8, 0, 1, 0, NULL},
    {NS_WX, "toff", "Temperature offset", SB_U8, 0, 20, 20, NULL},
    {NS_WX, "csv", "Weather log", SB_U8, 0, 1, 0, NULL},
};
_Static_assert(sizeof(keys) / sizeof(keys[0]) <= SB_KEYS_MAX, "the key table outgrew SB_KEYS_MAX");

#define OWN_COUNT (sizeof(keys) / sizeof(keys[0]))
#define APP_GROUPS 8

// The apps' groups (lichen_register_app, boot, core 1 or main: before anything exports); read-only afterwards
static struct {
    sb_key_t const *keys;
    size_t          n;
} s_groups[APP_GROUPS];
static size_t s_group_count, s_app_keys;

bool sb_add_app_keys(sb_key_t const *app_keys, size_t n) {
    if (!app_keys || n == 0 || s_group_count >= APP_GROUPS || OWN_COUNT + s_app_keys + n > SB_KEYS_MAX) {
        return false;
    }
    s_groups[s_group_count].keys = app_keys;
    s_groups[s_group_count].n    = n;
    s_group_count++;
    s_app_keys += n;
    return true;
}

void sb_app_keys_clear(void) {
    s_group_count = 0;
    s_app_keys    = 0;
}

size_t sb_key_count(void) {
    return OWN_COUNT + s_app_keys;
}

sb_key_t const *sb_key_at(size_t index) {
    if (index < OWN_COUNT) {
        return &keys[index];
    }
    index -= OWN_COUNT;
    for (size_t g = 0; g < s_group_count; g++) {
        if (index < s_groups[g].n) {
            return &s_groups[g].keys[index];
        }
        index -= s_groups[g].n;
    }
    return NULL;
}

sb_key_t const *sb_find(char const *ns, char const *key) {
    for (size_t i = 0; i < sb_key_count(); i++) {
        sb_key_t const *k = sb_key_at(i);
        if (strcmp(k->ns, ns) == 0 && strcmp(k->key, key) == 0) {
            return k;
        }
    }
    return NULL;
}

// A POSIX TZ string: names, numbers, signs, commas, dots, colons, slashes and <> for numeric names; no spaces
static bool tz_char(char c) {
    return isalnum((unsigned char)c) || strchr("+-:,./<>_", c) != NULL;
}

static bool str_valid(sb_key_t const *key, char const *text) {
    size_t n = strlen(text);
    if (n < key->min || n > key->max || n >= SB_STR_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!tz_char(text[i])) {
            return false;
        }
    }
    return true;
}

bool sb_value_parse(sb_key_t const *key, char const *text, sb_value_t *out) {
    memset(out, 0, sizeof(*out));
    out->key = key;
    if (key->type == SB_STR) {
        if (!str_valid(key, text)) {
            return false;
        }
        strcpy(out->str, text);
        return true;
    }
    if (!text[0] || strlen(text) > 3) {
        return false;
    }
    uint32_t v = 0;
    for (char const *p = text; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        v = v * 10 + (uint32_t)(*p - '0');
    }
    if (v < key->min || v > key->max) {
        return false;
    }
    out->num = v;
    return true;
}

void sb_value_text(sb_value_t const *value, char *out, size_t cap) {
    if (value->key->type == SB_STR) {
        snprintf(out, cap, "%s", value->str);
    } else {
        snprintf(out, cap, "%u", (unsigned)value->num);
    }
}

bool sb_value_equal(sb_value_t const *a, sb_value_t const *b) {
    return a->key == b->key && (a->key->type == SB_STR ? strcmp(a->str, b->str) == 0 : a->num == b->num);
}

size_t sb_format(char const *firmware, sb_value_t const *vals, size_t count, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap,
                     "# Lichen settings backup\n# firmware: %s\n"
                     "# One \"namespace.key = value\" per line; lines starting with # are ignored.\n"
                     "# Secrets (Wi-Fi and repeater passwords, the update token, the MeshCore identity and channel keys) are\n"
                     "# never saved here, and a line for a setting Lichen does not back up is skipped when loading.\n",
                     firmware && firmware[0] ? firmware : "unknown");
    size_t pos = n > 0 ? (size_t)n : cap;
    for (size_t i = 0; i < count && pos < cap; i++) {
        char text[SB_STR_MAX];
        sb_value_text(&vals[i], text, sizeof(text));
        n    = snprintf(out + pos, cap - pos, "%s.%s = %s\n", vals[i].key->ns, vals[i].key->key, text);
        pos += n > 0 ? (size_t)n : cap;
    }
    if (pos < cap) {
        n    = snprintf(out + pos, cap - pos, "%s", SB_END_LINE);
        pos += n > 0 ? (size_t)n : cap;
    }
    if (pos >= cap) {
        out[0] = '\0';
        return 0;
    }
    return pos;
}

bool sb_complete(char const *text, size_t len) {
    size_t e = sizeof(SB_END_LINE) - 1;
    return len >= e && memcmp(text + len - e, SB_END_LINE, e) == 0 && (len == e || text[len - e - 1] == '\n');
}

// ---- parsing ------------------------------------------------------------------------------------------------------

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

typedef enum { LINE_SKIP, LINE_IGNORED, LINE_VALUE, LINE_BAD_SYNTAX, LINE_BAD_VALUE } line_kind_t;

// One line (NUL terminated, modified): comment/blank, a key that is not ours, a valid value in *out, or an error
static line_kind_t parse_line(char *line, sb_value_t *out) {
    char *s = trim(line);
    if (!*s || *s == '#') {
        return LINE_SKIP;
    }
    char *eq = strchr(s, '=');
    if (!eq) {
        return LINE_BAD_SYNTAX;
    }
    *eq         = '\0';
    char *name  = trim(s);
    char *value = trim(eq + 1);
    char *dot   = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) {
        return LINE_BAD_SYNTAX;
    }
    *dot                  = '\0';
    sb_key_t const *key   = sb_find(name, dot + 1);
    if (!key) {
        return LINE_IGNORED; // also every secret: they are not in the table
    }
    return sb_value_parse(key, value, out) ? LINE_VALUE : LINE_BAD_VALUE;
}

static bool seen_key(sb_value_t const *out, size_t n, sb_key_t const *key) {
    for (size_t i = 0; i < n; i++) {
        if (out[i].key == key) {
            return true;
        }
    }
    return false;
}

sb_result_t sb_parse(char const *text, size_t len, sb_value_t *out, size_t max, size_t *count) {
    sb_result_t r = {SB_OK, 0, 0};
    size_t      n = 0, line_no = 0, pos = 0;
    *count = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            end++;
        }
        line_no++;
        char line[SB_LINE_MAX + 1];
        if (end - pos > SB_LINE_MAX) {
            return (sb_result_t){SB_ERR_TOO_LONG, line_no, r.ignored};
        }
        size_t n_line = end - pos;
        memcpy(line, text + pos, n_line);
        line[n_line] = '\0';
        if (memchr(text + pos, '\0', n_line)) {
            return (sb_result_t){SB_ERR_SYNTAX, line_no, r.ignored}; // a NUL inside: not a text file
        }
        pos = end + 1;
        sb_value_t v;
        switch (parse_line(line, &v)) {
            case LINE_SKIP: break;
            case LINE_IGNORED: r.ignored++; break;
            case LINE_BAD_SYNTAX: return (sb_result_t){SB_ERR_SYNTAX, line_no, r.ignored};
            case LINE_BAD_VALUE: return (sb_result_t){SB_ERR_VALUE, line_no, r.ignored};
            case LINE_VALUE:
                if (seen_key(out, n, v.key) || n >= max) {
                    return (sb_result_t){SB_ERR_DUP, line_no, r.ignored};
                }
                out[n++] = v;
                break;
        }
    }
    *count = n;
    if (n == 0) {
        r.err = SB_ERR_EMPTY;
    }
    return r;
}

sb_result_t sb_check_complete(char const *text, size_t len) {
    return (sb_result_t){sb_complete(text, len) ? SB_OK : SB_ERR_INCOMPLETE, 0, 0};
}

void sb_error_text(sb_result_t const *r, char *out, size_t cap) {
    switch (r->err) {
        case SB_OK: snprintf(out, cap, "The file is fine."); break;
        case SB_ERR_SYNTAX: snprintf(out, cap, "Line %u is not \"name = value\".", (unsigned)r->line); break;
        case SB_ERR_VALUE: snprintf(out, cap, "Line %u has a value that is not allowed.", (unsigned)r->line); break;
        case SB_ERR_DUP: snprintf(out, cap, "Line %u repeats a setting.", (unsigned)r->line); break;
        case SB_ERR_TOO_LONG: snprintf(out, cap, "Line %u is too long.", (unsigned)r->line); break;
        case SB_ERR_EMPTY: snprintf(out, cap, "The file has no settings Lichen can load."); break;
        case SB_ERR_INCOMPLETE: snprintf(out, cap, "The file was cut short."); break;
    }
}

void sb_summary(sb_value_t const *changed, size_t count, size_t ignored, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    int    n = snprintf(out, cap, "%u setting%s will change:", (unsigned)count, count == 1 ? "" : "s");
    size_t pos = n > 0 && (size_t)n < cap ? (size_t)n : cap - 1;
    for (size_t i = 0; i < count && i < 4 && pos < cap - 1; i++) {
        n    = snprintf(out + pos, cap - pos, "%s %s", i ? "," : "", changed[i].key->label);
        pos += n > 0 && (size_t)n < cap - pos ? (size_t)n : cap - 1 - pos;
    }
    if (count > 4 && pos < cap - 1) {
        n    = snprintf(out + pos, cap - pos, " and %u more", (unsigned)(count - 4));
        pos += n > 0 && (size_t)n < cap - pos ? (size_t)n : cap - 1 - pos;
    }
    if (ignored && pos < cap - 1) {
        snprintf(out + pos, cap - pos, ". %u line%s skipped.", (unsigned)ignored, ignored == 1 ? "" : "s");
    }
}
