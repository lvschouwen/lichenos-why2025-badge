#include "qr_payload.h"

#include <stdio.h>
#include <string.h>

static bool needs_escape(char c) {
    return c == '\\' || c == ';' || c == ',' || c == ':' || c == '"';
}

// Appends the escaped value; false when it does not fit (always leaving room for the final NUL)
static bool put_escaped(char *out, size_t cap, size_t *at, char const *value) {
    for (; *value; value++) {
        size_t need = needs_escape(*value) ? 2 : 1;
        if (*at + need >= cap) {
            return false;
        }
        if (need == 2) {
            out[(*at)++] = '\\';
        }
        out[(*at)++] = *value;
    }
    return true;
}

static bool put_text(char *out, size_t cap, size_t *at, char const *text) {
    size_t n = strlen(text);
    if (*at + n >= cap) {
        return false;
    }
    memcpy(out + *at, text, n);
    *at += n;
    return true;
}

bool qr_wifi_payload(char const *ssid, char const *password, char *out, size_t cap) {
    if (cap) {
        out[0] = '\0';
    }
    if (!cap || !ssid || !ssid[0]) {
        return false;
    }
    bool   open = !password || !password[0];
    size_t at   = 0;
    bool   ok   = put_text(out, cap, &at, open ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:") && put_escaped(out, cap, &at, ssid);
    if (ok && !open) {
        ok = put_text(out, cap, &at, ";P:") && put_escaped(out, cap, &at, password);
    }
    ok = ok && put_text(out, cap, &at, ";;");
    out[ok ? at : 0] = '\0';
    return ok;
}

bool qr_payload_ok(char const *text) {
    return text && text[0] && strlen(text) <= QR_PAYLOAD_MAX;
}
