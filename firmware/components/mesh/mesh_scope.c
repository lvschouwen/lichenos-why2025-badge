#include "mesh_scope.h"

#include <stdio.h>
#include <string.h>

char const *mesh_scope_parse(char const *in, char *out, size_t cap) {
    if (cap == 0) {
        return "no room";
    }
    out[0] = '\0';
    while (*in == ' ') {
        in++;
    }
    if (*in == '#') {
        in++;
    }
    size_t len = strlen(in);
    while (len > 0 && in[len - 1] == ' ') {
        len--;
    }
    if (len == 0) {
        return NULL; // None
    }
    if (len > MESH_SCOPE_MAX || len >= cap) {
        return "A region name is at most 30 characters";
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c >= 0x80;
        if (!ok) {
            out[0] = '\0';
            return "A region name takes letters, digits and '-'";
        }
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[len] = '\0';
    return NULL;
}

void mesh_scope_text(char const *name, char *out, size_t cap) {
    snprintf(out, cap, "%s", name && name[0] ? name : "None (everywhere)");
}

void mesh_scope_ask_text(char const *name, char *out, size_t cap) {
    snprintf(out, cap, "Adverts and messages will only travel in region %s. Contacts outside it can no longer reach you.",
             name);
}

void mesh_scope_key_text(char const *name, char *out, size_t cap) {
    snprintf(out, cap, "#%s", name);
}

mesh_scope_use_t mesh_scope_choose(bool from_console, char const *console_name, char const *channel_name) {
    if (from_console) {
        return console_name && console_name[0] ? MESH_SCOPE_USE_CONSOLE : MESH_SCOPE_USE_REFUSE;
    }
    if (!channel_name || !channel_name[0]) {
        return MESH_SCOPE_USE_DEFAULT;
    }
    return strcmp(channel_name, MESH_CHSCOPE_DAMAGED) == 0 ? MESH_SCOPE_USE_DAMAGED : MESH_SCOPE_USE_CHANNEL;
}

char const *mesh_scope_console_arg(char const *args, char *out, size_t cap) {
    if (cap == 0) {
        return "no room";
    }
    out[0]     = '\0';
    size_t len = strlen(args);
    while (len > 0 && args[len - 1] == ' ') {
        len--;
    }
    if (len < 3 || strncmp(args + len - 3, "YES", 3) != 0 || (len > 3 && args[len - 4] != ' ')) {
        return "this sets the scope of every flood from the console: add YES at the end, fw mesh scope console "
               "<region> YES";
    }
    char   region[MESH_SCOPE_MAX + 2];
    size_t region_len = len - 3;
    if (region_len >= sizeof(region)) {
        return "A region name is at most 30 characters";
    }
    memcpy(region, args, region_len);
    region[region_len] = '\0';
    char const *why    = mesh_scope_parse(region, out, cap);
    if (!why && !out[0]) {
        why = "mesh scope console <region> YES: a region name (an unset console scope refuses console floods)";
    }
    return why;
}

void mesh_scope_status_text(char const *default_name, char const *console_name, char *out, size_t cap) {
    snprintf(out, cap, "default %s | console %s", default_name && default_name[0] ? default_name : "None",
             console_name && console_name[0] ? console_name : "not set");
}

// ---- a channel's own scope (#291) -----------------------------------------------------------------------------------

void mesh_chscope_clear(mesh_chscope_t *t) {
    memset(t, 0, sizeof(*t));
}

static int chscope_index(mesh_chscope_t const *t, uint8_t const id[MESH_CHSCOPE_ID]) {
    for (size_t i = 0; i < t->n; i++) {
        if (memcmp(t->e[i].id, id, MESH_CHSCOPE_ID) == 0) {
            return (int)i;
        }
    }
    return -1;
}

char const *mesh_chscope_find(mesh_chscope_t const *t, uint8_t const id[MESH_CHSCOPE_ID]) {
    int i = chscope_index(t, id);
    return i < 0 ? "" : t->e[i].name;
}

void mesh_chscope_drop_at(mesh_chscope_t *t, size_t i) {
    if (i < t->n) {
        t->e[i] = t->e[--t->n]; // the order does not matter; what lies past n is never read
    }
}

bool mesh_chscope_put(mesh_chscope_t *t, uint8_t const id[MESH_CHSCOPE_ID], char const *name) {
    int i = chscope_index(t, id);
    if (!name[0]) {
        if (i >= 0) {
            mesh_chscope_drop_at(t, (size_t)i);
        }
        return true;
    }
    if (i < 0) {
        if (t->n >= MESH_CHSCOPE_MAX) {
            return false;
        }
        i = (int)t->n++;
        memcpy(t->e[i].id, id, MESH_CHSCOPE_ID);
    }
    snprintf(t->e[i].name, sizeof(t->e[i].name), "%s", name);
    return true;
}

size_t mesh_chscope_write(mesh_chscope_t const *t, char *out, size_t cap) {
    size_t len = 0;
    if (cap) {
        out[0] = '\0';
    }
    for (size_t i = 0; i < t->n; i++) {
        char line[2 * MESH_CHSCOPE_ID + MESH_SCOPE_MAX + 3];
        int  n = 0;
        for (size_t b = 0; b < MESH_CHSCOPE_ID; b++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x", t->e[i].id[b]);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s\n", t->e[i].name);
        if (len + (size_t)n >= cap) {
            if (cap) {
                out[0] = '\0';
            }
            return 0;
        }
        memcpy(out + len, line, (size_t)n + 1);
        len += (size_t)n;
    }
    return len;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)(c | 0x20); // lower case
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// One "<16 hex> <region>" line (len bytes, no line end) into the table; false when its id does not read. ended: the
// line had its line end (a cut write leaves the last one without, maybe shorter: a tombstone, never a shorter region).
static bool chscope_read_line(mesh_chscope_t *t, char const *line, size_t len, bool ended) {
    uint8_t id[MESH_CHSCOPE_ID];
    char    region[MESH_SCOPE_MAX + 2], name[MESH_SCOPE_MAX + 1];
    if (len < 2 * MESH_CHSCOPE_ID) {
        return false;
    }
    for (size_t b = 0; b < MESH_CHSCOPE_ID; b++) {
        int hi = hex_digit(line[2 * b]), lo = hex_digit(line[2 * b + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        id[b] = (uint8_t)(hi << 4 | lo);
    }
    size_t rlen = len > 2 * MESH_CHSCOPE_ID ? len - (2 * MESH_CHSCOPE_ID + 1) : 0;
    bool   ok   = ended && len > 2 * MESH_CHSCOPE_ID + 1 && line[2 * MESH_CHSCOPE_ID] == ' ' && rlen < sizeof(region);
    if (ok) {
        memcpy(region, line + 2 * MESH_CHSCOPE_ID + 1, rlen);
        region[rlen] = '\0';
        ok           = !mesh_scope_parse(region, name, sizeof(name)) && name[0];
    }
    return mesh_chscope_put(t, id, ok ? name : MESH_CHSCOPE_DAMAGED);
}

size_t mesh_chscope_read(mesh_chscope_t *t, char const *text) {
    size_t lines = 0;
    mesh_chscope_clear(t);
    while (*text) {
        size_t len  = strcspn(text, "\n");
        size_t used = len;
        if (used > 0 && text[used - 1] == '\r') {
            used--;
        }
        lines += chscope_read_line(t, text, used, text[len] == '\n');
        text += len + (text[len] == '\n');
    }
    return lines;
}

mesh_chscope_file_t mesh_chscope_file_pick(bool have_file, bool have_tmp) {
    if (have_file) {
        return MESH_CHSCOPE_FILE_READ; // a temp file next to it is a write cut before its rename: the old one holds
    }
    return have_tmp ? MESH_CHSCOPE_FILE_PROMOTE : MESH_CHSCOPE_FILE_NONE;
}

void mesh_scope_channel_text(char const *own, char const *default_name, char *out, size_t cap) {
    if (own && strcmp(own, MESH_CHSCOPE_DAMAGED) == 0) {
        snprintf(out, cap, "Damaged: set it again");
    } else if (own && own[0]) {
        snprintf(out, cap, "%s", own);
    } else if (default_name && default_name[0]) {
        snprintf(out, cap, "Default (%s)", default_name);
    } else {
        snprintf(out, cap, "Default (None, everywhere)");
    }
}

void mesh_scope_channel_ask_text(char const *channel, char const *name, char *out, size_t cap) {
    snprintf(out, cap,
             "Messages you send on %s are sent to region %s. Repeaters outside it do not pass them on, so members who "
             "only reach you through them stop receiving them.",
             channel, name);
}
