#include "lichen_manifest.h"

#include "app_caps.h"
#include "fs_path.h"
#include "lichen_api_version.h"
#include "lichen_caps.h"

#include <string.h>

bool lichen_api_compatible(unsigned app_major, unsigned app_minor) {
    return app_major == LICHEN_API_VERSION_MAJOR && app_minor <= LICHEN_API_VERSION_MINOR;
}

char const *lichen_manifest_err_text(lichen_manifest_err_t err) {
    static char const *const T[] = {"ok",     "bad argument",    "bad line",   "unknown key", "duplicate key",
                                    "bad id", "bad value",       "bad api version", "missing key"};
    return (unsigned)err < sizeof(T) / sizeof(T[0]) ? T[err] : "?";
}

enum { K_ID, K_NAME, K_API, K_ENTRY, K_ICON, K_CAPS, K_IRAM, K_PSRAM, K_STACK, K_COUNT };
static char const *const KEYS[K_COUNT] = {"id",   "name", "api",              "entry",     "icon",
                                          "caps", "internal_ram_kib", "psram_kib", "stack_kib"};

// Digits only, 1..5 of them, up to 65535; *end moves past them
static bool parse_u16(char const *s, char const **end, uint16_t *out) {
    uint32_t v = 0;
    size_t   n = 0;
    for (; s[n] >= '0' && s[n] <= '9'; n++) {
        if (n >= 5) {
            return false;
        }
        v = v * 10 + (uint32_t)(s[n] - '0');
    }
    if (n == 0 || v > 65535) {
        return false;
    }
    *out = (uint16_t)v;
    *end = s + n;
    return true;
}

static bool parse_version(char const *s, uint16_t *major, uint16_t *minor) {
    char const *p;
    if (!parse_u16(s, &p, major) || *p != '.') {
        return false;
    }
    return parse_u16(p + 1, &p, minor) && *p == '\0';
}

// A file next to the manifest: no path, no leading dot, plain characters
static bool file_name_ok(char const *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= LICHEN_MANIFEST_NAME_MAX || s[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.')) {
            return false;
        }
    }
    return true;
}

static bool name_ok(char const *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= LICHEN_MANIFEST_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) {
            return false;
        }
    }
    return s[0] != ' ' && s[n - 1] != ' ';
}

static bool parse_caps(char const *s, uint32_t *out) {
    uint32_t caps = 0;
    for (;;) {
        s += strspn(s, " ,");
        size_t n = strcspn(s, " ,");
        if (n == 0) {
            break;
        }
        unsigned bit = 0;
        for (; bit < CAP_COUNT; bit++) {
            char const *name = app_caps_name(bit);
            if (strlen(name) == n && strncmp(name, s, n) == 0) {
                break;
            }
        }
        if (bit >= CAP_COUNT || (1u << bit) == CAP_SYSTEM) { // SYSTEM is the OS's own apps
            return false;
        }
        caps |= 1u << bit;
        s += n;
    }
    *out = caps;
    return true;
}

static void copy(char *dst, size_t cap, char const *src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static lichen_manifest_err_t set_text(lichen_manifest_t *m, int key, char const *v, char const *dir_id) {
    char probe[32];
    switch (key) {
    case K_ID:
        if (!fs_app_dir_path("/x", v, probe, sizeof(probe)) || !dir_id || strcmp(v, dir_id) != 0) {
            return LICHEN_MANIFEST_ERR_ID;
        }
        copy(m->id, sizeof(m->id), v);
        return LICHEN_MANIFEST_OK;
    case K_NAME:
        copy(m->name, sizeof(m->name), v);
        return name_ok(v) ? LICHEN_MANIFEST_OK : LICHEN_MANIFEST_ERR_VALUE;
    case K_ENTRY:
        copy(m->entry, sizeof(m->entry), v);
        return file_name_ok(v) ? LICHEN_MANIFEST_OK : LICHEN_MANIFEST_ERR_VALUE;
    default: // K_ICON
        copy(m->icon, sizeof(m->icon), v);
        return file_name_ok(v) ? LICHEN_MANIFEST_OK : LICHEN_MANIFEST_ERR_VALUE;
    }
}

static lichen_manifest_err_t set_value(lichen_manifest_t *m, int key, char const *v, char const *dir_id) {
    switch (key) {
    case K_ID:
    case K_NAME:
    case K_ENTRY:
    case K_ICON:
        return set_text(m, key, v, dir_id);
    case K_API:
        return parse_version(v, &m->api_major, &m->api_minor) ? LICHEN_MANIFEST_OK : LICHEN_MANIFEST_ERR_VERSION;
    case K_CAPS:
        return parse_caps(v, &m->caps) ? LICHEN_MANIFEST_OK : LICHEN_MANIFEST_ERR_VALUE;
    default: {
        uint16_t    n;
        char const *end;
        if (!parse_u16(v, &end, &n) || *end) {
            return LICHEN_MANIFEST_ERR_VALUE;
        }
        *(key == K_IRAM ? &m->internal_ram_kib : key == K_PSRAM ? &m->psram_kib : &m->stack_kib) = n;
        return LICHEN_MANIFEST_OK;
    }
    }
}

// One line without its line end: skips blanks and comments, else splits at the first '=' and stores the value
static lichen_manifest_err_t parse_line(char *line, lichen_manifest_t *m, unsigned *seen, char const *dir_id) {
    if (line[0] == '\0' || line[0] == '#') {
        return LICHEN_MANIFEST_OK;
    }
    char *eq = strchr(line, '=');
    if (!eq || eq == line) {
        return LICHEN_MANIFEST_ERR_LINE;
    }
    *eq     = '\0';
    int key = -1;
    for (int k = 0; k < K_COUNT; k++) {
        if (strcmp(line, KEYS[k]) == 0) {
            key = k;
        }
    }
    if (key < 0) {
        return LICHEN_MANIFEST_ERR_KEY;
    }
    if (*seen & (1u << key)) {
        return LICHEN_MANIFEST_ERR_DUPLICATE;
    }
    *seen |= 1u << key;
    return set_value(m, key, eq + 1, dir_id);
}

lichen_manifest_err_t lichen_manifest_parse(char const *text, size_t len, char const *dir_id, lichen_manifest_t *out,
                                            unsigned *err_line) {
    unsigned dummy;
    if (!err_line) {
        err_line = &dummy;
    }
    *err_line = 0;
    if (!text || !out) {
        return LICHEN_MANIFEST_ERR_ARG;
    }
    lichen_manifest_t m;
    memset(&m, 0, sizeof(m));
    copy(m.entry, sizeof(m.entry), "app.elf");
    unsigned seen = 0, lineno = 0;
    size_t   pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            end++;
        }
        size_t n = end - pos;
        lineno++;
        if (n > 0 && text[pos + n - 1] == '\r') {
            n--;
        }
        char line[LICHEN_MANIFEST_LINE_MAX + 1];
        // A NUL inside a line would end it early and hide the rest from the checks
        if (n > LICHEN_MANIFEST_LINE_MAX || memchr(text + pos, '\0', n) != NULL) {
            *err_line = lineno;
            return LICHEN_MANIFEST_ERR_LINE;
        }
        memcpy(line, text + pos, n);
        line[n]                 = '\0';
        lichen_manifest_err_t e = parse_line(line, &m, &seen, dir_id);
        if (e != LICHEN_MANIFEST_OK) {
            *err_line = lineno;
            return e;
        }
        pos = end + 1;
    }
    if (!(seen & (1u << K_ID)) || !(seen & (1u << K_NAME)) || !(seen & (1u << K_API))) {
        return LICHEN_MANIFEST_ERR_MISSING;
    }
    *out = m;
    return LICHEN_MANIFEST_OK;
}
