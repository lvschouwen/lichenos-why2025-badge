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

// Strict UTF-8 as FatFs decodes it: no stray continuation, no overlong form, no surrogate, nothing above U+10FFFF
static bool utf8_ok(char const *s) {
    unsigned char const *p = (unsigned char const *)s;
    while (*p) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        int      n  = *p >= 0xF0 ? 3 : *p >= 0xE0 ? 2 : *p >= 0xC2 ? 1 : 0;
        uint32_t cp = n == 3 ? *p & 0x07u : n == 2 ? *p & 0x0Fu : *p & 0x1Fu;
        if (n == 0 || *p > 0xF4) {
            return false;
        }
        for (int i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return false;
            }
            cp = cp << 6 | (p[i] & 0x3Fu);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        p += n + 1;
    }
    return true;
}

bool files_name_ok_fat(char const *name) {
    if (!files_name_ok(name) || !utf8_ok(name)) { // files_name_ok already refuses DEL with the other control characters
        return false;
    }
    for (char const *p = name; *p; p++) {
        if (strchr("\"*:<>?|\\", *p)) {
            return false;
        }
    }
    char last = name[strlen(name) - 1];
    return last != ' ' && last != '.';
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

// Names at the /storage root that are the OS's (lora.toml: the radio, ota_token.txt: the update token, the four
// directories it creates; every directory there is protected anyway, see files_protected_entry).
// Anything else at the root is the user's, e.g. what the Files app copies there.
static char const *const STORAGE_ROOT_FILES[] = {"lora.toml", "ota_token.txt", "c6", "crash", "lichen"};
// Secret material: never shown, not even read-only (the update token)
static char const *const SECRET_ROOT[] = {"ota_token.txt"};

// The apps' roots (lichen_app_t.storage_root, #230 E1d), added when Files opens: protected like the OS's, and secret
// when the app says so (MeshCore's identity and keys). Only ever added to, so a job reading while the screen opens is safe.
#define APP_ROOT_MAX 16
#define APP_ROOT_CAP 32
static struct {
    char name[APP_ROOT_CAP];
    bool secret;
} s_app_root[APP_ROOT_MAX];
static size_t s_app_roots;

bool files_add_app_root(char const *name, bool secret) {
    if (!name || !*name || strlen(name) >= APP_ROOT_CAP) {
        return false;
    }
    for (size_t i = 0; i < s_app_roots; i++) {
        if (!strcmp(s_app_root[i].name, name)) {
            s_app_root[i].secret = s_app_root[i].secret || secret;
            return true;
        }
    }
    if (s_app_roots >= APP_ROOT_MAX) {
        return false;
    }
    strcpy(s_app_root[s_app_roots].name, name);
    s_app_root[s_app_roots].secret = secret;
    s_app_roots++;
    return true;
}

void files_app_roots_clear(void) {
    s_app_roots = 0;
}

static bool app_root(char const *name, size_t len, bool secret_only) {
    for (size_t i = 0; i < s_app_roots; i++) {
        if ((!secret_only || s_app_root[i].secret) && same_comp(name, len, s_app_root[i].name)) {
            return true;
        }
    }
    return false;
}

static bool in_list(char const *const *list, size_t n, char const *name, size_t len) {
    for (size_t i = 0; i < n; i++) {
        if (same_comp(name, len, list[i])) {
            return true;
        }
    }
    return false;
}

// The path below the volume root: the number of components and the first one. false when the path is not ok.
static bool split(char const *path, char const **vol, size_t *n, char const **first, size_t *first_len) {
    if (!files_path_ok(path)) {
        return false;
    }
    *vol         = files_volume_of(path);
    char const *rest = path + strlen(*vol);
    *n           = 0;
    *first       = NULL;
    *first_len   = 0;
    for (char const *p = *rest ? rest + 1 : rest; *p;) {
        char const *end = strchr(p, '/');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        if (!*n) {
            *first     = p;
            *first_len = len;
        }
        (*n)++;
        p += len + (end ? 1 : 0);
    }
    return true;
}

// Deny by default. /storage is the OS's and the apps': every directory in it and everything below one is protected,
// as are the root files in STORAGE_ROOT_FILES; only other plain files at the root are the user's (see
// files_protected_entry for the directory case). On /sd the whole /lichen tree is protected, the rest is the user's.
bool files_protected(char const *path) {
    char const *vol, *first;
    size_t      n, first_len;
    if (!split(path, &vol, &n, &first, &first_len) || n == 0) {
        return true; // not a usable path, or a volume root
    }
    if (has_alias(first, first_len)) {
        return true; // could be a short name for anything
    }
    if (!strcmp(vol, FILES_SD)) {
        return same_comp(first, first_len, "lichen");
    }
    if (n >= 2) {
        return true;
    }
    return in_list(STORAGE_ROOT_FILES, sizeof(STORAGE_ROOT_FILES) / sizeof(STORAGE_ROOT_FILES[0]), first, first_len) ||
           app_root(first, first_len, false) || !strncmp(first, "ota_", 4) || !strncmp(first, "lora", 4);
}

bool files_protected_entry(char const *path, bool is_dir) {
    char const *vol, *first;
    size_t      n, first_len;
    if (files_protected(path)) {
        return true;
    }
    // a directory straight under /storage is an OS or app directory, whatever its name
    return is_dir && split(path, &vol, &n, &first, &first_len) && !strcmp(vol, FILES_STORAGE) && n == 1;
}

bool files_secret(char const *path) {
    char const *vol, *first;
    size_t      n, first_len;
    if (!split(path, &vol, &n, &first, &first_len) || n == 0) {
        return true;
    }
    if (!strcmp(vol, FILES_SD)) {
        return has_alias(first, first_len) || (same_comp(first, first_len, "lichen") && n >= 2); // the journal and app data on the card: not for the screen
    }
    return has_alias(first, first_len) ||
           in_list(SECRET_ROOT, sizeof(SECRET_ROOT) / sizeof(SECRET_ROOT[0]), first, first_len) ||
           app_root(first, first_len, true);
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

// Where the page is: the line being filled and the last place it can be broken (after a space, #144)
typedef struct {
    sink_t sink;
    size_t used, ctrl, seen; // input bytes taken, control bytes shown as '.', characters shown
    int    line, col;        // col in width units
    bool   can_break;
    size_t brk_out, brk_used, brk_ctrl, brk_seen; // the state at the break: the line goes on from brk_used
    bool   skip_blanks; // just wrapped: blanks at the start of the new line are dropped
} page_t;

static bool wrap_line(page_t *p, int lines, bool at_space) {
    if (p->can_break && !at_space) { // back to the last blank: the word moves to the next line whole
        p->sink.used = p->brk_out;
        p->used      = p->brk_used;
        p->ctrl      = p->brk_ctrl;
        p->seen      = p->brk_seen;
    }
    p->can_break   = false;
    p->skip_blanks = true;
    p->col         = 0;
    return ++p->line < lines && put(&p->sink, "\n", 1);
}

// Width of the character in units: 1 each, or the caller's measure (pixels) so a proportional font fills its lines
static int cell_width(files_width_fn measure, void *ctx, char const *shown, size_t n) {
    int w = measure ? measure(ctx, shown, n) : 1;
    return w > 0 ? w : 1;
}

// One input character as it is shown: bytes in shown, take input bytes, bad when shown as '.' or '?' (counted once the
// character is placed, not when it is retried)
typedef struct {
    char   shown[4];
    size_t take;
    bool   bad;
} glyph_t;

// A newline ends the line; false when the page is full (or the sink is)
static bool take_newline(page_t *p, int lines) {
    if (!put(&p->sink, "\n", 1)) {
        return false;
    }
    p->used++;
    if (++p->line >= lines) {
        return false;
    }
    p->col         = 0;
    p->can_break   = false;
    p->skip_blanks = false;
    return true;
}

// Decodes the character at p->used; false when it is cut off by the end of the buffer (the next read completes it)
static bool read_glyph(uint8_t const *buf, size_t len, bool at_eof, page_t const *p, glyph_t *g) {
    uint8_t c = buf[p->used];
    g->take   = 1;
    g->bad    = false;
    if (c == '\t') {
        g->shown[0] = ' ';
    } else if (control(c)) {
        g->shown[0] = '.';
        g->bad      = true;
    } else if (c < 0x80) {
        g->shown[0] = (char)c;
    } else {
        int u = utf8_len(buf + p->used, len - p->used);
        if (u < 0 && !at_eof) {
            return false;
        }
        if (u > 0) {
            memcpy(g->shown, buf + p->used, (size_t)u);
            g->take = (size_t)u;
        } else {
            g->shown[0] = '?';
            g->bad      = true;
        }
    }
    return true;
}

// Puts the character on the line, or wraps first when it does not fit; false when the page is full
static bool place_glyph(page_t *p, glyph_t const *g, int cols, int lines, files_width_fn measure, void *measure_ctx) {
    size_t width = g->take;
    int    w     = cell_width(measure, measure_ctx, g->shown, width);
    bool   blank = g->shown[0] == ' ';
    if (p->col > 0 && p->col + w > cols) { // does not fit: wrap at the last blank, or inside a word too long for a line
        if (blank) {
            p->used += g->take; // the blank at the edge is the break itself
        }
        return wrap_line(p, lines, blank); // a word is read again from the start of the new line
    }
    if (blank && p->col > 0 && p->sink.used > 0 && p->sink.out[p->sink.used - 1] != ' ') {
        p->can_break = true; // a break before this blank; the line resumes after it
        p->brk_out   = p->sink.used;
        p->brk_used  = p->used + g->take;
        p->brk_ctrl  = p->ctrl;
        p->brk_seen  = p->seen;
    }
    if (!put(&p->sink, g->shown, width)) {
        return false;
    }
    p->col += w;
    p->ctrl += g->bad;
    p->used += g->take;
    p->seen++;
    return true;
}

size_t files_text_page(files_text_in_t const *in, files_text_out_t const *o) {
    char *const out = o->out;
    page_t      p   = {.sink = {out, o->cap, 0}};
    if (o->cap) {
        out[0] = '\0';
    }
    if (!o->cap || in->cols < 1 || in->lines < 1) {
        *o->consumed = 0;
        *o->binary   = false;
        return 0;
    }
    while (p.used < in->len) {
        uint8_t c = in->buf[p.used];
        if (c == '\r') {
            p.used++;
        } else if (c == '\n') {
            if (!take_newline(&p, in->lines)) {
                break;
            }
        } else if ((c == ' ' || c == '\t') && p.skip_blanks) {
            p.used++;
        } else {
            glyph_t g;
            p.skip_blanks = false;
            if (!read_glyph(in->buf, in->len, in->at_eof, &p, &g) ||
                !place_glyph(&p, &g, in->cols, in->lines, in->measure, in->measure_ctx)) {
                break;
            }
        }
    }
    out[p.sink.used] = '\0';
    *o->consumed     = p.used;
    *o->binary       = (p.seen + p.ctrl) > 0 && p.ctrl * 10 > p.used;
    return p.sink.used;
}
