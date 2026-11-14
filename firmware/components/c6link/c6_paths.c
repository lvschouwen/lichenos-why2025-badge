#include "c6_paths.h"

#include <stdio.h>

#include <string.h>
#include <strings.h>

bool c6_image_path(char const *file, char *out, size_t cap) {
    if (!*file || strchr(file, '/') || strstr(file, "..")) {
        return false;
    }
    int n = snprintf(out, cap, "%s/%s", C6_IMAGE_DIR, file);
    return n > 0 && (size_t)n < cap;
}

bool c6_path_in_image_dir(char const *path) {
    static char const *const dir[] = {"storage", "c6"}; // C6_IMAGE_DIR's parts (c6_paths_test checks they agree)
    size_t                   k     = 0;
    bool                     same  = true;
    for (char const *p = path; *p;) {
        if (*p == '/') {
            p++;
            continue;
        }
        size_t len = strcspn(p, "/");
        if (p[0] == '.' && (len == 1 || (len == 2 && p[1] == '.'))) {
            return true;
        }
        if (same && k < 2) {
            same = strlen(dir[k]) == len && strncasecmp(p, dir[k], len) == 0;
            k++;
        }
        p += len;
    }
    return same && k == 2;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool c6_md5_matches(char const *hex, uint8_t const digest[16]) {
    for (int i = 0; i < 16; i++) {
        int hi = hex_value(hex[2 * i]);
        int lo = hi < 0 ? -1 : hex_value(hex[2 * i + 1]); // stops at the NUL of a short string
        if (lo < 0 || ((hi << 4) | lo) != digest[i]) {
            return false;
        }
    }
    return true;
}
