// The bounded string copy: lichen_str.h. Pure; no strnlen, which a host build in strict C11 does not declare.
#include "lichen_str.h"

#include <string.h>

bool lichen_str_copy_n(char *dst, size_t cap, char const *src, size_t n) {
    size_t len = 0;
    while (src && len < n && src[len]) {
        len++;
    }
    if (cap == 0) {
        return len > 0;
    }
    size_t keep = len < cap - 1 ? len : cap - 1;
    if (keep) { // a NULL src has len 0
        memcpy(dst, src, keep);
    }
    dst[keep] = '\0';
    return keep < len;
}

bool lichen_str_copy(char *dst, size_t cap, char const *src) {
    return lichen_str_copy_n(dst, cap, src, (size_t)-1);
}
