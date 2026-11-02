#include "files_model.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool control(unsigned char c) {
    return c < 0x20 || c == 0x7F;
}

bool files_name_ok(char const *name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) >= FILES_NAME_MAX) {
        return false;
    }
    for (char const *p = name; *p; p++) {
        if (*p == '/' || control((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

char const *files_volume_of(char const *path) {
    if (!path) {
        return NULL;
    }
    size_t s = strlen(FILES_STORAGE), d = strlen(FILES_SD);
    if (!strncmp(path, FILES_STORAGE, s) && (path[s] == '/' || !path[s])) {
        return FILES_STORAGE;
    }
    if (!strncmp(path, FILES_SD, d) && (path[d] == '/' || !path[d])) {
        return FILES_SD;
    }
    return NULL;
}

char const *files_other_volume(char const *volume) {
    return volume && !strcmp(volume, FILES_SD) ? FILES_STORAGE : FILES_SD;
}

bool files_path_ok(char const *path) {
    char const *vol = files_volume_of(path);
    if (!vol || strlen(path) >= FILES_PATH_MAX) {
        return false;
    }
    char const *p = path + strlen(vol);
    while (*p) { // p is at a '/'
        p++;
        char const *end = strchr(p, '/');
        size_t      n   = end ? (size_t)(end - p) : strlen(p);
        char        comp[FILES_NAME_MAX];
        if (n == 0 || n >= sizeof(comp)) {
            return false;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        if (!files_name_ok(comp)) {
            return false;
        }
        p += n;
    }
    return true;
}

bool files_path_join(char const *dir, char const *name, char *out, size_t cap) {
    if (!dir || !out || !files_name_ok(name) || !files_path_ok(dir)) {
        return false;
    }
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap || !files_path_ok(out)) {
        if (cap) {
            out[0] = '\0';
        }
        return false;
    }
    return true;
}

bool files_path_parent(char const *path, char *out, size_t cap) {
    if (!files_path_ok(path) || !out || !cap) {
        return false;
    }
    char const *vol = files_volume_of(path);
    if (!strcmp(path, vol)) {
        out[0] = '\0';
        return true;
    }
    size_t n = strrchr(path, '/') - path;
    if (n >= cap) {
        return false;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

// ---- protected paths ----------------------------------------------------------------------------------------------

// FAT ignores trailing dots and spaces and matches a long name by its 8.3 alias ("MESHCO~1") too
static size_t trimmed_len(char const *s, size_t n) {
    while (n && (s[n - 1] == '.' || s[n - 1] == ' ')) {
        n--;
    }
    return n;
}

static bool has_alias(char const *s, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] == '~' && isdigit((unsigned char)s[i + 1])) {
            return true;
        }
    }
    return false;
}

static bool same_comp(char const *a, size_t an, char const *b) {
    an = trimmed_len(a, an);
    if (an != strlen(b)) {
        return false;
    }
    for (size_t i = 0; i < an; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

typedef struct {
    char const *path; // below the volume root, components separated by '/'
    bool        tree; // everything under it too
} rule_t;

static rule_t const STORAGE_RULES[] = {
    {"meshcore", true},       // identity, contacts, channels
    {"c6", true},             // the staged C6 images
    {"crash", true},          // the crash log
    {"lichen", true},         // notifications and Lichen's own state
    {"lora.toml", false},     // radio settings
    {"ota_token.txt", false}, // the update token
};
static rule_t const SD_RULES[] = {
    {"lichen", false},          // the directory itself; the apps' own directories below can be cleaned
    {"lichen/apps", false},
    {"lichen/messages", true},  // MeshCore's message history
};

// Does the component list `comps` (n entries) match the rule: equal, or below it for a tree rule
static bool rule_hit(rule_t const *r, char const *const *comps, size_t const *lens, size_t n) {
    char const *p = r->path;
    size_t      i = 0;
    while (*p) {
        char const *slash = strchr(p, '/');
        size_t      len   = slash ? (size_t)(slash - p) : strlen(p);
        char        part[24];
        memcpy(part, p, len);
        part[len] = '\0';
        if (i >= n || !same_comp(comps[i], lens[i], part)) {
            return false;
        }
        i++;
        p += len + (slash ? 1 : 0);
    }
    return r->tree ? true : i == n;
}

bool files_protected(char const *path) {
    if (!files_path_ok(path)) {
        return true;
    }
    char const *vol  = files_volume_of(path);
    char const *rest = path + strlen(vol);
    if (!*rest) {
        return true; // the volume roots
    }
    char const *comps[FILES_PATH_MAX / 2];
    size_t      lens[FILES_PATH_MAX / 2];
    size_t      n = 0;
    for (char const *p = rest + 1; *p && n < FILES_PATH_MAX / 2;) {
        char const *end = strchr(p, '/');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        if (has_alias(p, len)) {
            return true; // could be a short name for anything
        }
        comps[n] = p;
        lens[n]  = len;
        n++;
        p += len + (end ? 1 : 0);
    }
    bool                 sd    = !strcmp(vol, FILES_SD);
    rule_t const        *rules = sd ? SD_RULES : STORAGE_RULES;
    size_t               count = sd ? sizeof(SD_RULES) / sizeof(SD_RULES[0]) : sizeof(STORAGE_RULES) / sizeof(STORAGE_RULES[0]);
    for (size_t i = 0; i < count; i++) {
        if (rule_hit(&rules[i], comps, lens, n)) {
            return true;
        }
    }
    return false;
}

// ---- sorting ------------------------------------------------------------------------------------------------------

static int cmp_entries(void const *pa, void const *pb) {
    files_entry_t const *a = pa, *b = pb;
    if (a->dir != b->dir) {
        return a->dir ? -1 : 1;
    }
    for (size_t i = 0;; i++) {
        int ca = tolower((unsigned char)a->name[i]), cb = tolower((unsigned char)b->name[i]);
        if (ca != cb) {
            return ca - cb;
        }
        if (!ca) {
            break;
        }
    }
    return strcmp(a->name, b->name);
}

void files_sort(files_entry_t *entries, size_t count) {
    if (entries && count > 1) {
        qsort(entries, count, sizeof(*entries), cmp_entries);
    }
}

// ---- text paging --------------------------------------------------------------------------------------------------

// Length of a valid UTF-8 sequence at s (1..4), 0 when invalid; -1 when it is cut off by the end of the buffer
static int utf8_len(uint8_t const *s, size_t left) {
    uint8_t c = s[0];
    size_t  need = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (c > 0xF4 || !need) {
        return 0;
    }
    for (size_t i = 1; i < need; i++) {
        if (i >= left) {
            return -1;
        }
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return (int)need;
}

typedef struct {
    char  *out;
    size_t cap, used;
} sink_t;

static bool put(sink_t *s, char const *bytes, size_t n) {
    if (s->used + n + 1 > s->cap) {
        return false;
    }
    memcpy(s->out + s->used, bytes, n);
    s->used += n;
    return true;
}

size_t files_text_page(uint8_t const *buf, size_t len, bool at_eof, int cols, int lines, char *out, size_t cap,
                       size_t *consumed, bool *binary) {
    size_t used = 0, ctrl = 0, seen = 0;
    int    line = 0, col = 0;
    sink_t sink = {out, cap, 0};
    if (cap) {
        out[0] = '\0';
    }
    if (!cap || cols < 1 || lines < 1) {
        *consumed = 0;
        *binary   = false;
        return 0;
    }
    while (used < len) {
        uint8_t c = buf[used];
        if (c == '\r') {
            used++;
            continue;
        }
        if (c == '\n') {
            if (!put(&sink, "\n", 1)) {
                break;
            }
            used++;
            if (++line >= lines) {
                break;
            }
            col = 0;
            continue;
        }
        char    shown[4];
        size_t  width = 1, take = 1;
        if (c == '\t') {
            shown[0] = ' ';
        } else if (control(c)) {
            shown[0] = '.';
            ctrl++;
        } else if (c < 0x80) {
            shown[0] = (char)c;
        } else {
            int u = utf8_len(buf + used, len - used);
            if (u < 0 && !at_eof) {
                break; // cut off by the end of the buffer: the next read completes it
            }
            if (u > 0) {
                memcpy(shown, buf + used, (size_t)u);
                width = take = (size_t)u;
            } else {
                shown[0] = '?';
                ctrl++;
            }
        }
        if (col >= cols) { // wrap before this character
            if (++line >= lines || !put(&sink, "\n", 1)) {
                break;
            }
            col = 0;
        }
        if (!put(&sink, shown, width)) {
            break;
        }
        col++;
        used += take;
        seen++;
    }
    out[sink.used] = '\0';
    *consumed      = used;
    *binary        = (seen + ctrl) > 0 && ctrl * 10 > used;
    return sink.used;
}
