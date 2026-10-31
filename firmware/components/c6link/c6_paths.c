#include "c6_paths.h"

#include <stdio.h>

#include <string.h>

bool c6_image_path(char const *file, char *out, size_t cap) {
    if (!*file || strchr(file, '/') || strstr(file, "..")) {
        return false;
    }
    int n = snprintf(out, cap, "%s/%s", C6_IMAGE_DIR, file);
    return n > 0 && (size_t)n < cap;
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
