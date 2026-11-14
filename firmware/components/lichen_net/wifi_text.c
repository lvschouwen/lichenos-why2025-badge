#include "wifi_text.h"

#include <string.h>

void wifi_text_copy(char *dst, size_t cap, char const *src) {
    if (!dst || cap == 0) {
        return;
    }
    size_t n = src ? strnlen(src, cap - 1) : 0;
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}
