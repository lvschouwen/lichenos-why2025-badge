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

// The next '/'-separated part of *p (empty parts skipped); false at the end
static bool next_part(char const **p, char const **part, size_t *len) {
    while (**p == '/') {
        (*p)++;
    }
    *part = *p;
    while (**p && **p != '/') {
        (*p)++;
    }
    *len = (size_t)(*p - *part);
    return *len > 0;
}

bool c6_path_in_image_dir(char const *path) {
    char const *dir = C6_IMAGE_DIR, *part, *want;
    size_t      len, want_len;
    bool        same = true;
    while (next_part(&path, &part, &len)) {
        if (part[0] == '.' && (len == 1 || (len == 2 && part[1] == '.'))) {
            return true;
        }
        if (same && next_part(&dir, &want, &want_len)) {
            same = len == want_len && strncasecmp(part, want, len) == 0;
        }
    }
    return same && !next_part(&dir, &want, &want_len);
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
