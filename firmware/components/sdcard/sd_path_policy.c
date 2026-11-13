// Which files "fw sd cat" may read (#378): see sd_path_policy.h
#include "sd_path_policy.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

// MeshCore's directory on /storage (meshcore_app_record.h, shared/arduino_shim/SPIFFS.h) and what in it is secret:
// identity/_main.id (+ ".id.new" while a save is in flight, IdentityStore.cpp) holds the private key, channels2 the
// channel keys, new_prefs (and the older prefs.json) may hold a Wi-Fi password (NodePrefs.wifi_pwd)
#define MESHCORE_DIR "meshcore"
static char const *const MESHCORE_SECRET[] = {"identity", "channels2", "new_prefs", "prefs.json"};
// Anywhere: the update token (ota_server.c TOKEN_FILE, ota_token.txt) and anything named like it
static char const *const DENY_PREFIX[] = {"ota_token", "nvs"}; // nvs: a partition dump (docs/nvs-encryption.md)
static char const *const DENY_SUFFIX[] = {".key", ".pem", ".id", ".id.new"};

typedef struct {
    char const *p;
    size_t      n;
} comp_t;

static bool ci_eq(comp_t c, char const *s) {
    size_t n = strlen(s);
    if (c.n != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)c.p[i]) != tolower((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static bool ci_prefix(comp_t c, char const *s) {
    size_t n = strlen(s);
    return c.n >= n && ci_eq((comp_t){c.p, n}, s);
}

static bool ci_suffix(comp_t c, char const *s) {
    size_t n = strlen(s);
    return c.n >= n && ci_eq((comp_t){c.p + c.n - n, n}, s);
}

// Bytes no name of ours holds, which some file system reads as something else: FatFs takes '\\' as a separator and
// "0:" as a drive, '*' and '?' are wildcards to some tools, and a control byte could rewrite the console line
static bool bad_byte(unsigned char c) {
    return c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c) != NULL;
}

// ".", "..", and what FAT quietly turns into another name: trailing dots and spaces are dropped ("ota_token.txt."),
// a leading space is kept but invisible, and "~1" may be an 8.3 alias of any long name ("OTA_TO~1.TXT")
static bool bad_comp(comp_t c) {
    if (c.n == 0 || c.p[0] == ' ' || c.p[c.n - 1] == '.' || c.p[c.n - 1] == ' ') {
        return true; // also "." and "..": they end in a dot
    }
    for (size_t i = 0; i + 1 < c.n; i++) {
        if (c.p[i] == '~' && isdigit((unsigned char)c.p[i + 1])) {
            return true;
        }
    }
    return false;
}

static bool in_list(comp_t c, char const *const *list, size_t n, bool (*match)(comp_t, char const *)) {
    for (size_t i = 0; i < n; i++) {
        if (match(c, list[i])) {
            return true;
        }
    }
    return false;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// What the checks need of a path: its first three components, its last, how many, and whether any is named like a
// denied prefix (kept small: this runs on the 4 KB console task)
typedef struct {
    comp_t head[3];
    comp_t last;
    size_t count;
    bool   prefix_denied;
} parsed_t;

// false for anything not plain: "//", a trailing '/', a dot trick, a bad byte
static bool split(char const *path, parsed_t *out) {
    if (path[0] != '/') {
        return false;
    }
    for (char const *s = path; *s; s++) {
        if (bad_byte((unsigned char)*s)) {
            return false;
        }
    }
    *out = (parsed_t){0};
    for (char const *p = path + 1;;) {
        char const *end = strchr(p, '/');
        comp_t      c   = {p, end ? (size_t)(end - p) : strlen(p)};
        if (bad_comp(c)) {
            return false;
        }
        if (out->count < COUNT(out->head)) {
            out->head[out->count] = c;
        }
        out->count++;
        out->last           = c;
        out->prefix_denied |= in_list(c, DENY_PREFIX, COUNT(DENY_PREFIX), ci_prefix); // a directory named so too
        if (!end) {
            return true;
        }
        p = end + 1;
    }
}

static bool denied(parsed_t const *p) {
    if (p->prefix_denied || in_list(p->last, DENY_SUFFIX, COUNT(DENY_SUFFIX), ci_suffix)) {
        return true;
    }
    return p->count >= 3 && ci_eq(p->head[0], "storage") && ci_eq(p->head[1], MESHCORE_DIR) &&
           in_list(p->head[2], MESHCORE_SECRET, COUNT(MESHCORE_SECRET), ci_eq);
}

static bool exact(comp_t c, char const *s) {
    return c.n == strlen(s) && !memcmp(c.p, s, c.n);
}

sd_path_verdict_t sd_path_check(char const *path) {
    if (!path) {
        return SD_PATH_BAD;
    }
    if (strnlen(path, SD_PATH_MAX + 1) > SD_PATH_MAX) {
        return SD_PATH_TOO_LONG;
    }
    parsed_t p;
    if (!split(path, &p)) {
        return SD_PATH_BAD;
    }
    if (denied(&p)) {
        return SD_PATH_DENIED; // before the roots: the deny list wins
    }
    // The VFS matches mount points exactly, so "/SD" is not the card; a volume root is a directory, never a file
    bool root = exact(p.head[0], "sd") || exact(p.head[0], "storage");
    return root && p.count >= 2 ? SD_PATH_OK : SD_PATH_ROOT;
}

char const *sd_path_verdict_text(sd_path_verdict_t v) {
    switch (v) {
        case SD_PATH_OK: return "ok";
        case SD_PATH_BAD: return "bad path";
        case SD_PATH_ROOT: return "outside /sd and /storage";
        case SD_PATH_TOO_LONG: return "path too long";
        case SD_PATH_DENIED: return "denied";
    }
    return "bad path";
}
