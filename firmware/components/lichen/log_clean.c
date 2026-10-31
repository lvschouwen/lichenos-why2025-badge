#include "log_clean.h"

size_t log_clean(char const *in, size_t len, char *out) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\033' && i + 1 < len && in[i + 1] == '[') {
            i += 2;
            while (i < len && (in[i] < '@' || in[i] > '~')) {
                i++;
            }
            continue;
        }
        if (in[i] == '\r' || in[i] == '\n') {
            continue;
        }
        out[n++] = in[i];
    }
    out[n] = '\0';
    return n;
}
