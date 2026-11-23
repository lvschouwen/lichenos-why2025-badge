#include "fs_path.h"

#include "app_ns.h"

#include <stdio.h>
#include <string.h>

// Folders the OS keeps on the drives: the C6 image, the crash log (#294), the notification store. Pinned against every
// STORAGE_BASE/SD_BASE folder in the tree by tools/tests/reserved_ids_test.py.
#define FS_SANDBOX_COMPONENT_MAX 100 // a FAT long name is at most 255; an app has no use for more

static char const *const RESERVED_IDS[] = {"c6", "crash", "lichen"};

static bool id_reserved(char const *id) {
    for (size_t i = 0; id && i < sizeof(RESERVED_IDS) / sizeof(RESERVED_IDS[0]); i++) {
        if (strcmp(id, RESERVED_IDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool fs_app_dir_path(char const *base, char const *id, char *out, size_t cap) {
    char ns[16];
    if (!base || base[0] != '/' || id_reserved(id) || !app_ns_name(id, ns, sizeof(ns))) {
        return false;
    }
    size_t need = strlen(base) + 1 + strlen(id) + 1;
    if (!out || cap < need) {
        return false;
    }
    snprintf(out, cap, "%s/%s", base, id);
    return true;
}

// One component of the app's path: false when FAT would read it as something else than it says
static bool component_ok(char const *c, size_t n) {
    if (n == 0 || n > FS_SANDBOX_COMPONENT_MAX) {
        return false;
    }
    if (c[n - 1] == '.' || c[n - 1] == ' ') {
        return false; // also "..": FatFs strips trailing dots and spaces, so ".. " and "..." are ".." to it
    }
    for (size_t i = 0; i < n; i++) {
        if (c[i] == '\\' || c[i] == ':' || (unsigned char)c[i] < ' ') {
            return false;
        }
    }
    return true;
}

static bool resolve_into(char const *dir, char const *path, bool with_root, char *out, size_t cap) {
    if (!dir || !path || !out || dir[0] != '/' || !path[0]) {
        return false;
    }
    size_t dir_len = strlen(dir);
    if (dir_len < 2 || dir[dir_len - 1] == '/') {
        return false; // the dir itself is a plain absolute path with a name
    }
    char const *rest = path;
    if (path[0] == '/') {
        if (strncmp(path, dir, dir_len) != 0 || (path[dir_len] != '/' && path[dir_len] != '\0')) {
            return false; // elsewhere, or a sibling sharing the prefix ("/x/hello2" for "/x/hello")
        }
        rest = path + dir_len;
    }
    size_t used = dir_len;
    if (dir_len + 1 > cap) {
        return false;
    }
    memcpy(out, dir, dir_len);
    out[dir_len] = '\0';
    for (char const *p = rest; *p;) {
        while (*p == '/') {
            p++;
        }
        size_t n = strcspn(p, "/");
        if (n == 0) {
            break;
        }
        if (n == 1 && p[0] == '.') {
            p += n;
            continue;
        }
        if (!component_ok(p, n) || used + 1 + n + 1 > cap) {
            return false;
        }
        out[used++] = '/';
        memcpy(out + used, p, n);
        used += n;
        out[used] = '\0';
        p += n;
    }
    return with_root || used > dir_len;
}

bool fs_sandbox_resolve(char const *dir, char const *path, bool with_root, char *out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    bool ok = resolve_into(dir, path, with_root, out, cap);
    if (!ok) {
        out[0] = '\0'; // never half a path
    }
    return ok;
}
