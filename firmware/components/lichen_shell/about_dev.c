#include "about_dev.h"

#include <stdbool.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The value part is everything before the final " (...)" age suffix; a text without one is all value
static size_t value_len(char const *raw) {
    size_t len = strlen(raw);
    if (len && raw[len - 1] == ')') {
        for (size_t i = len - 1; i-- > 0;) {
            if (raw[i] == ' ' && raw[i + 1] == '(') {
                return i;
            }
        }
    }
    return len;
}

// A plain decimal number, or false (a "?" or a name stays as the registry wrote it)
static bool parse_u(char const *s, size_t n, uint32_t *out) {
    if (!n || n > 10) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void show_value(char const *key, char const *v, size_t n, char *out, size_t cap) {
    uint32_t num;
    if (!n) {
        snprintf(out, cap, "none");
    } else if (strcmp(key, "disp.refresh") == 0 && parse_u(v, n, &num)) {
        snprintf(out, cap, "%" PRIu32 ".%" PRIu32 " Hz", num / 10, num % 10);
    } else if (strcmp(key, "c6.reg_caps") == 0 && parse_u(v, n, &num)) { // a bit set: hex reads, 32790 does not (#344)
        snprintf(out, cap, "0x%" PRIx32, num);
    } else if (strcmp(key, "psram.clk") == 0 && parse_u(v, n, &num)) {
        snprintf(out, cap, "%" PRIu32 " MHz", (num + 500000) / 1000000);
    } else {
        snprintf(out, cap, "%.*s", (int)n, v);
    }
}

size_t about_dev_text(char const *key, char const *raw, char *out, size_t cap) {
    if (!cap) {
        return 0;
    }
    if (raw && !*raw && strcmp(key, "settings.last_err") == 0) {
        return (size_t)snprintf(out, cap, "none"); // no failure: the registry's value is an empty string (#344)
    }
    if (!raw || !*raw) {
        return (size_t)snprintf(out, cap, "not available");
    }
    if (raw[0] == '?') {
        return (size_t)snprintf(out, cap, "%s", raw); // "? (not read yet)": the reason is the answer
    }
    size_t n = value_len(raw);
    char   val[64];
    show_value(key, raw, n, val, sizeof(val));
    return (size_t)snprintf(out, cap, "%s%s", val, raw + n);
}
