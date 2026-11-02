// A repeater's settings over its CLI (#117): see mesh_rset.h
#include "mesh_rset.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PASSWORD_MAX 15 // = MF_PASSWORD_MAX: the repeater keeps 15 characters (char password[16])
#define NAME_MAX     31 // simple_repeater's node_name[32]

// The order is the screen's. Ranges are simple_repeater's own checks; tx has none upstream, -9..30 dBm covers the
// boards it runs on.
static rs_row_t const ROWS[RS_ROWS] = {
    {"name", "Name", RS_TEXT, false, 0, 0, NULL, NULL},
    {"radio", "Radio", RS_RADIO, false, 0, 0, NULL, NULL},
    {"tx", "TX power", RS_INT, false, -9, 30, "dBm", NULL},
    {"repeat", "Repeat", RS_ONOFF, false, 0, 0, NULL, NULL},
    {"advert.interval", "Advert every", RS_INT, true, 60, 240, "min", NULL},
    {"flood.advert.interval", "Flood advert every", RS_INT, true, 3, 168, "h", NULL},
    {"flood.max", "Flood max hops", RS_INT, false, 0, 64, NULL, NULL},
    {"af", "Airtime factor", RS_DEC, false, 0, 9, NULL, NULL},
    {"txdelay", "TX delay (flood)", RS_DEC, false, 0, 2, NULL, NULL},
    {"direct.txdelay", "TX delay (direct)", RS_DEC, false, 0, 2, NULL, NULL},
    {"rxdelay", "RX delay", RS_DEC, false, 0, 20, NULL, NULL},
    {"lat", "Latitude", RS_DEC, false, -90, 90, NULL, NULL},
    {"lon", "Longitude", RS_DEC, false, -180, 180, NULL, NULL},
    {"allow.read.only", "Read-only logins", RS_ONOFF, false, 0, 0, NULL, NULL},
    {"loop.detect", "Loop detection", RS_CHOICE, false, 0, 0, NULL, "off|minimal|moderate|strict"},
    {"role", "Role", RS_READONLY, false, 0, 0, NULL, NULL},
    {"ver", "Firmware", RS_READONLY, false, 0, 0, NULL, NULL},
    {"password", "Admin password", RS_PASSWORD, false, 0, 0, NULL, NULL},
    {"reboot", "Reboot", RS_REBOOT, false, 0, 0, NULL, NULL},
};

size_t rs_row_count(void) {
    return RS_ROWS;
}

rs_row_t const *rs_row_at(size_t i) {
    return i < RS_ROWS ? &ROWS[i] : NULL;
}

int rs_row_find(char const *key) {
    for (size_t i = 0; i < RS_ROWS; i++) {
        if (strcmp(ROWS[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool rs_row_readable(size_t i) {
    return i < RS_ROWS && ROWS[i].kind != RS_PASSWORD && ROWS[i].kind != RS_REBOOT;
}

bool rs_row_editable(size_t i) {
    return i < RS_ROWS && ROWS[i].kind != RS_READONLY;
}

// value without the spaces around it, into out; false when it does not fit
static bool trimmed(char const *value, char *out, size_t cap) {
    while (*value == ' ') {
        value++;
    }
    size_t n = strlen(value);
    while (n && value[n - 1] == ' ') {
        n--;
    }
    if (n + 1 > cap) {
        return false;
    }
    memcpy(out, value, n);
    out[n] = 0;
    return true;
}

static bool has_control(char const *s) {
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7F) {
            return true;
        }
    }
    return false;
}

// Only plain decimal numbers ([-]digits[.digits]): the repeater reads the text with atoi/atof, which would take "3e1",
// "0x10" or ".5e1" otherwise than strtod does, and the text is what is sent
static bool plain_number(char const *s) {
    if (*s == '-') {
        s++;
    }
    size_t whole = strspn(s, "0123456789");
    if (!whole) {
        return false;
    }
    s += whole;
    if (*s == '.') {
        size_t frac = strspn(s + 1, "0123456789");
        if (!frac) {
            return false;
        }
        s += 1 + frac;
    }
    return *s == 0;
}

static bool whole_number(char const *s, double *out) {
    char *end = NULL;
    if (!plain_number(s)) {
        return false;
    }
    double v = strtod(s, &end);
    if (*end || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

// The radio's four numbers as the text has them, split at commas and spaces
static int radio_parts(char const *text, char parts[4][16]) {
    int n = 0;
    while (*text) {
        while (*text == ',' || *text == ' ') {
            text++;
        }
        if (!*text) {
            break;
        }
        size_t len = strcspn(text, ", ");
        if (n == 4 || len >= 16) {
            return -1;
        }
        memcpy(parts[n], text, len);
        parts[n][len] = 0;
        n++;
        text += len;
    }
    return n;
}

bool rs_radio_parse(char const *text, float *freq, float *bw, uint8_t *sf, uint8_t *cr) {
    char   parts[4][16];
    double v[4];
    if (radio_parts(text, parts) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!whole_number(parts[i], &v[i])) {
            return false;
        }
    }
    if (v[0] < 150 || v[0] > 2500 || v[1] < 7 || v[1] > 500 || v[2] != floor(v[2]) || v[3] != floor(v[3]) ||
        v[2] < 5 || v[2] > 12 || v[3] < 5 || v[3] > 8) {
        return false;
    }
    *freq = (float)v[0];
    *bw   = (float)v[1];
    *sf   = (uint8_t)v[2];
    *cr   = (uint8_t)v[3];
    return true;
}

static bool is_choice(char const *choices, char const *value) {
    size_t n = strlen(value);
    for (char const *c = choices; *c;) {
        size_t len = strcspn(c, "|");
        if (len == n && n && strncmp(c, value, n) == 0) {
            return true;
        }
        c += len + (c[len] == '|');
    }
    return false;
}

char const *rs_check(size_t i, char const *value) {
    if (i >= RS_ROWS) {
        return "no such setting";
    }
    rs_row_t const *row = &ROWS[i];
    char            v[RS_CMD_MAX + 1];
    if (row->kind == RS_REBOOT) {
        return NULL;
    }
    if (row->kind == RS_READONLY) {
        return "read only";
    }
    if (row->kind == RS_PASSWORD) {
        size_t n = strlen(value);
        if (n == 0 || n > PASSWORD_MAX) {
            return "1 to 15 characters";
        }
        if (value[0] == ' ' || value[n - 1] == ' ' || has_control(value)) {
            return "no spaces at the start or end";
        }
        return NULL;
    }
    if (!trimmed(value, v, sizeof(v)) || has_control(v)) {
        return "too long";
    }
    double d;
    float  f, bw;
    uint8_t sf, cr;
    switch (row->kind) {
    case RS_TEXT:
        if (!*v || strlen(v) > NAME_MAX) {
            return "1 to 31 characters";
        }
        return strpbrk(v, "[]\\:,?*") ? "none of [ ] \\ : , ? *" : NULL;
    case RS_INT:
        if (!whole_number(v, &d) || d != floor(d)) {
            return "a whole number";
        }
        if (row->zero_ok && d == 0) {
            return NULL;
        }
        if (d < row->min || d > row->max) {
            static char why[48];
            snprintf(why, sizeof(why), "%s%d to %d", row->zero_ok ? "0 (off) or " : "", (int)row->min, (int)row->max);
            return why;
        }
        return NULL;
    case RS_DEC:
        if (!whole_number(v, &d)) {
            return "a number";
        }
        if (d < row->min || d > row->max) {
            static char why[48];
            snprintf(why, sizeof(why), "%g to %g", (double)row->min, (double)row->max);
            return why;
        }
        return NULL;
    case RS_ONOFF: return strcmp(v, "on") == 0 || strcmp(v, "off") == 0 ? NULL : "on or off";
    case RS_CHOICE: return is_choice(row->choices, v) ? NULL : "off, minimal, moderate or strict";
    case RS_RADIO:
        return rs_radio_parse(v, &f, &bw, &sf, &cr) ? NULL : "MHz,kHz,SF,CR (150-2500, 7-500, 5-12, 5-8)";
    default: return "not settable";
    }
}

static bool put(char *out, size_t cap, char const *tag, char const *body, char const *value) {
    int n = snprintf(out, cap, "%s%s%s%s%s", tag ? tag : "", tag ? "|" : "", body, value ? " " : "", value ? value : "");
    if (n < 0 || (size_t)n >= cap || n > RS_CMD_MAX) {
        if (cap) {
            memset(out, 0, cap); // a password must not stay behind half written
        }
        return false;
    }
    return true;
}

bool rs_get_cmd(size_t i, char const *tag, char *out, size_t cap) {
    if (!rs_row_readable(i)) {
        return false;
    }
    if (strcmp(ROWS[i].key, "ver") == 0) {
        return put(out, cap, tag, "ver", NULL); // a command of its own, not a get
    }
    char body[40];
    snprintf(body, sizeof(body), "get %s", ROWS[i].key);
    return put(out, cap, tag, body, NULL);
}

bool rs_set_cmd(size_t i, char const *value, char const *tag, char *out, size_t cap) {
    if (i >= RS_ROWS || rs_check(i, value)) {
        return false;
    }
    rs_row_t const *row = &ROWS[i];
    if (row->kind == RS_REBOOT) {
        return put(out, cap, tag, "reboot", NULL);
    }
    if (row->kind == RS_PASSWORD) {
        return put(out, cap, tag, "password", value);
    }
    char v[RS_CMD_MAX + 1], body[40];
    trimmed(value, v, sizeof(v));
    if (row->kind == RS_RADIO) {
        char parts[4][16];
        radio_parts(v, parts); // checked above: four numbers, sent with the commas the repeater splits at
        snprintf(v, sizeof(v), "%s,%s,%s,%s", parts[0], parts[1], parts[2], parts[3]);
    }
    snprintf(body, sizeof(body), "set %s", row->key);
    return put(out, cap, tag, body, v);
}

// ---- replies ----------------------------------------------------------------------------------------------------------

static bool tagged(char const *line, size_t len) {
    return len >= 3 && line[2] == '|';
}

// The line of the reply that answers tag (or the first one, untagged mode): its start and length
static bool our_line(char const *reply, char const *tag, char const **at, size_t *len) {
    for (char const *line = reply; *line;) {
        size_t n = strcspn(line, "\n");
        if (!tag) {
            *at  = line;
            *len = n;
            return true;
        }
        if (tagged(line, n) && line[0] == tag[0] && line[1] == tag[1]) {
            *at  = line + 3;
            *len = n - 3;
            return true;
        }
        line += n + (line[n] == '\n');
    }
    return false;
}

static void copy_trimmed(char const *s, size_t n, char *out, size_t cap) {
    while (n && *s == ' ') {
        s++;
        n--;
    }
    while (n && s[n - 1] == ' ') {
        n--;
    }
    if (!cap) {
        return;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s, n);
    out[n] = 0;
}

static bool starts(char const *s, size_t n, char const *with) {
    size_t w = strlen(with);
    return n >= w && strncasecmp(s, with, w) == 0;
}

rs_reply_t rs_parse_reply(size_t i, bool is_set, char const *tag, char const *reply, char *out, size_t cap) {
    char const *line;
    size_t      n;
    if (cap) {
        out[0] = 0;
    }
    if (i >= RS_ROWS || !reply || !our_line(reply, tag, &line, &n)) {
        return RS_R_NOT_OURS;
    }
    if (is_set) {
        if (ROWS[i].kind == RS_PASSWORD && starts(line, n, "password now:")) {
            copy_trimmed("Password changed", 16, out, cap); // the echo of the password itself is not kept
            return RS_R_OK;
        }
        copy_trimmed(line, n, out, cap);
        return starts(line, n, "ok") ? RS_R_OK : RS_R_ERROR;
    }
    if (strcmp(ROWS[i].key, "ver") == 0) {
        if (starts(line, n, "unknown") || n == 0) {
            return RS_R_UNSUPPORTED;
        }
        copy_trimmed(line, n, out, cap);
        return RS_R_VALUE;
    }
    if (n >= 1 && line[0] == '>') {
        copy_trimmed(line + 1, n - 1, out, cap);
        return RS_R_VALUE;
    }
    return RS_R_UNSUPPORTED; // "??: key", "unknown config", "Unknown command", "Error: unsupported", "null"
}

// ---- the filter -------------------------------------------------------------------------------------------------------

static char const *skip_tag(char const *cmd) {
    while (*cmd == ' ') {
        cmd++;
    }
    if (strlen(cmd) > 4 && cmd[2] == '|') {
        cmd += 3; // as simple_repeater reads it
    }
    return cmd;
}

static char const *const SECRET[] = {"password ", "set guest.password ", "set prv.key "};

rs_cmd_class_t rs_cmd_class(char const *cmd) {
    static char const *const RADIO[]  = {"set radio ", "set freq ", "tempradio "};
    static char const *const REBOOT[] = {"reboot", "clkreboot", "poweroff", "shutdown", "start ota"};
    char const *c = skip_tag(cmd);
    if (strncmp(c, "erase", 5) == 0 && (c[5] == 0 || c[5] == ' ')) {
        return RS_CMD_FORBIDDEN;
    }
    for (size_t k = 0; k < sizeof(RADIO) / sizeof(RADIO[0]); k++) {
        if (strncmp(c, RADIO[k], strlen(RADIO[k])) == 0) {
            return RS_CMD_RADIO;
        }
    }
    for (size_t k = 0; k < sizeof(SECRET) / sizeof(SECRET[0]); k++) {
        if (strncmp(c, SECRET[k], strlen(SECRET[k])) == 0) {
            return RS_CMD_SECRET;
        }
    }
    for (size_t k = 0; k < sizeof(REBOOT) / sizeof(REBOOT[0]); k++) {
        if (strncmp(c, REBOOT[k], strlen(REBOOT[k])) == 0) {
            return RS_CMD_REBOOT;
        }
    }
    return RS_CMD_PLAIN;
}

void rs_redact_cmd(char const *cmd, char *out, size_t cap) {
    char const *c = skip_tag(cmd);
    if (!cap) {
        return;
    }
    for (size_t k = 0; k < sizeof(SECRET) / sizeof(SECRET[0]); k++) {
        size_t len = strlen(SECRET[k]);
        if (strncmp(c, SECRET[k], len) == 0) {
            snprintf(out, cap, "%.*s (hidden)", (int)(c - cmd + len - 1), cmd);
            return;
        }
    }
    snprintf(out, cap, "%s", cmd);
}

void rs_redact_reply(char const *reply, char *out, size_t cap) {
    static char const ECHO[] = "password now:";
    char const       *at     = strstr(reply, ECHO);
    if (!cap) {
        return;
    }
    if (at) {
        snprintf(out, cap, "%.*s (hidden)", (int)(at - reply + sizeof(ECHO) - 1), reply);
    } else {
        snprintf(out, cap, "%s", reply);
    }
}

// ---- the sequence -----------------------------------------------------------------------------------------------------

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}

void rs_init(rs_t *s) {
    wipe(s, sizeof(*s));
    s->set_row = s->cur_row = s->last_row = -1;
    s->last                               = RS_R_NOT_OURS;
}

void rs_read_all(rs_t *s) {
    s->misses  = 0;
    s->stopped = false;
    memset(s->tries, 0, sizeof(s->tries));
    for (size_t i = 0; i < RS_ROWS; i++) {
        if (rs_row_readable(i) && !(s->cur_row == (int)i && !s->cur_set)) {
            s->want |= 1u << i;
            s->st[i] = RS_V_UNKNOWN;
        }
    }
}

char const *rs_queue_set(rs_t *s, size_t i, char const *value) {
    if (s->set_row >= 0 || (s->cur_row >= 0 && s->cur_set)) {
        return "the last change is still waiting for its answer";
    }
    char const *why = rs_check(i, value);
    if (why) {
        return why;
    }
    if (strlen(value) > RS_CMD_MAX) {
        return "too long";
    }
    snprintf(s->set_value, sizeof(s->set_value), "%s", value);
    s->set_row  = (int8_t)i;
    s->last_row = -1; // the outcome shown next is this set's
    s->last     = RS_R_NOT_OURS;
    return NULL;
}

bool rs_busy(rs_t const *s) {
    return s->cur_row >= 0 || s->set_row >= 0 || s->want;
}

static void next_tag(rs_t *s) {
    static char const DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    s->tag_n++;
    s->cur_tag[0] = DIGITS[s->tag_n / 36 % 36];
    s->cur_tag[1] = DIGITS[s->tag_n % 36];
    s->cur_tag[2] = 0;
}

static void set_last(rs_t *s, int row, rs_reply_t r, char const *text) {
    s->last_row = (int8_t)row;
    s->last     = (uint8_t)r;
    snprintf(s->last_text, sizeof(s->last_text), "%s", text);
}

bool rs_next(rs_t *s, char *out, size_t cap) {
    while (s->cur_row < 0 && (s->set_row >= 0 || s->want)) {
        next_tag(s);
        char const *tag   = s->untagged ? NULL : s->cur_tag;
        bool        probe = !s->untagged && !s->probed; // the first command is a get: it learns whether tags work
        bool        ok;
        if (probe && !s->want) {
            s->want |= 1u << 0; // a set queued before any get: the name is read first
        }
        if (s->set_row >= 0 && !probe) {
            int row  = s->set_row;
            ok       = rs_set_cmd((size_t)row, s->set_value, tag, out, cap);
            s->set_row = -1;
            wipe(s->set_value, sizeof(s->set_value)); // on its way: a password is not kept here
            if (!ok) {
                set_last(s, row, RS_R_ERROR, "Does not fit in a command");
                continue;
            }
            s->cur_row = (int8_t)row;
            s->cur_set = true;
        } else {
            int row = __builtin_ctz(s->want);
            s->want &= ~(1u << row);
            ok = rs_get_cmd((size_t)row, tag, out, cap);
            s->st[row] = ok ? RS_V_ASKING : RS_V_NONE;
            if (!ok) {
                continue;
            }
            s->cur_row = (int8_t)row;
            s->cur_set = false;
        }
        s->tag_probe = probe && !s->cur_set;
        s->probed    = s->probed || s->tag_probe;
        return true;
    }
    return false;
}

// An untagged "Unknown command": a repeater that does not strip the tag read "a7|get name" as one unknown word
static bool untagged_refusal(char const *reply) {
    for (char const *line = reply; *line;) {
        size_t n = strcspn(line, "\n");
        if (n && !tagged(line, n) && starts(line, n, "unknown command")) {
            return true;
        }
        line += n + (line[n] == '\n');
    }
    return false;
}

static void stop_reading(rs_t *s) {
    for (size_t i = 0; i < RS_ROWS; i++) {
        if (s->want & (1u << i)) {
            s->st[i] = RS_V_NONE;
        }
    }
    s->want    = 0;
    s->stopped = true;
}

static void end_current(rs_t *s) {
    s->cur_row   = -1;
    s->cur_set   = false;
    s->tag_probe = false;
}

bool rs_on_answer(rs_t *s, char const *reply) {
    int row = s->cur_row;
    if (row < 0) {
        return true;
    }
    if (!reply) {
        if (!s->cur_set) {
            s->st[row] = RS_V_NONE;
            if (++s->misses >= RS_MISSES_MAX) {
                stop_reading(s); // the repeater is out of reach: no point in asking the rest one by one
            }
        } else if (ROWS[row].kind == RS_REBOOT) {
            set_last(s, row, RS_R_OK, "Reboot sent (a repeater does not answer it)");
        } else {
            set_last(s, row, RS_R_ERROR, "No answer: it may or may not have taken it");
            if (rs_row_readable((size_t)row)) {
                s->want |= 1u << row; // read it back to see
            }
        }
        end_current(s);
        return true;
    }
    char       text[RS_TEXT_MAX];
    rs_reply_t r = rs_parse_reply((size_t)row, s->cur_set, s->untagged ? NULL : s->cur_tag, reply, text, sizeof(text));
    if (r == RS_R_NOT_OURS) {
        if (!(s->tag_probe && untagged_refusal(reply))) {
            return false;
        }
        s->untagged = true; // this repeater does not reflect the tag: everything again without one
        if (s->cur_set) {
            set_last(s, row, RS_R_ERROR, "The repeater did not understand it: try again");
        } else {
            s->want |= 1u << row;
            s->st[row] = RS_V_UNKNOWN;
        }
        end_current(s);
        return true;
    }
    s->misses = 0;
    if (!s->cur_set) {
        s->st[row] = r == RS_R_VALUE ? RS_V_VALUE : RS_V_NONE;
        snprintf(s->value[row], RS_VALUE_MAX, "%.*s", RS_VALUE_MAX - 1, r == RS_R_VALUE ? text : "");
    } else {
        set_last(s, row, r, text);
        if (r == RS_R_OK && rs_row_readable((size_t)row)) {
            s->want |= 1u << row; // read back: what the repeater now has
            s->st[row] = RS_V_UNKNOWN;
        }
    }
    end_current(s);
    return true;
}

void rs_on_unsent(rs_t *s, char const *why) {
    int row = s->cur_row;
    if (row < 0) {
        return;
    }
    if (s->cur_set) {
        char text[RS_TEXT_MAX];
        snprintf(text, sizeof(text), "Not sent: %s", why ? why : "?");
        set_last(s, row, RS_R_ERROR, text);
    } else if (++s->tries[row] < RS_UNSENT_MAX) {
        s->want |= 1u << row; // asked again (after the others): MeshCore may have been busy
        s->st[row] = RS_V_UNKNOWN;
    } else {
        s->st[row] = RS_V_NONE;
    }
    if (s->tag_probe) {
        s->probed = false; // the probe did not go out: the next get probes
    }
    end_current(s);
}

void rs_value_text(rs_t const *s, size_t i, char *out, size_t cap) {
    if (i >= RS_ROWS || !cap) {
        return;
    }
    rs_row_t const *row = &ROWS[i];
    if (row->kind == RS_REBOOT) {
        snprintf(out, cap, "%s", "");
    } else if (row->kind == RS_PASSWORD) {
        snprintf(out, cap, "change");
    } else if (s->st[i] == RS_V_VALUE) {
        bool unit = row->unit && s->value[i][0];
        snprintf(out, cap, "%s%s%s", s->value[i], unit ? " " : "", unit ? row->unit : "");
    } else if (s->st[i] == RS_V_NONE) {
        snprintf(out, cap, "\xE2\x80\x94"); // an em dash: no answer
    } else {
        snprintf(out, cap, "...");
    }
}

void rs_edit_text(rs_t const *s, size_t i, char *out, size_t cap) {
    if (!cap) {
        return;
    }
    snprintf(out, cap, "%s", i < RS_ROWS && s->st[i] == RS_V_VALUE ? s->value[i] : "");
}

void rs_progress(rs_t const *s, size_t *done, size_t *total) {
    *done = *total = 0;
    for (size_t i = 0; i < RS_ROWS; i++) {
        if (rs_row_readable(i)) {
            (*total)++;
            *done += s->st[i] == RS_V_VALUE || s->st[i] == RS_V_NONE;
        }
    }
}
