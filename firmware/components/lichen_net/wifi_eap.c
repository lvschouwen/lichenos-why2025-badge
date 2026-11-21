#include "wifi_eap.h"

#include <string.h>

#define US   '\x1f'
#define VER0 'E'
#define VER1 '1'

bool wifi_eap_text_valid(char const *s, size_t min, size_t max) {
    if (!s) {
        return false;
    }
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (n >= max || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return n >= min;
}

static bool has_suffix(char const *s, char const *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n > m && strcmp(s + n - m, suffix) == 0;
}

bool wifi_eap_ca_path_valid(char const *path) {
    if (!path || !path[0]) {
        return path != NULL;
    }
    size_t dir = strlen(WIFI_EAP_CA_DIR);
    // A backslash is no separator on /storage but a path-walking trick elsewhere: refused outright
    if (strchr(path, '\\') || !wifi_eap_text_valid(path, dir + 1, WIFI_EAP_CA_MAX) ||
        strncmp(path, WIFI_EAP_CA_DIR, dir) != 0 ||
        !(has_suffix(path, ".pem") || has_suffix(path, ".crt"))) {
        return false;
    }
    // Every segment after /storage/ is a name: "..", "." and "//" would walk out of the directory or name nothing
    for (char const *seg = path + dir; *seg;) {
        char const *end = strchr(seg, '/');
        size_t      len = end ? (size_t)(end - seg) : strlen(seg);
        if (len == 0 || (len == 1 && seg[0] == '.') || (len == 2 && seg[0] == '.' && seg[1] == '.')) {
            return false;
        }
        seg += len + (end ? 1 : 0);
    }
    return true;
}

bool wifi_eap_valid(wifi_eap_t const *e) {
    if (!e || e->method > WIFI_EAP_TTLS || e->phase2 > WIFI_EAP_P2_PAP) {
        return false;
    }
    if (e->method == WIFI_EAP_PEAP && e->phase2 != WIFI_EAP_P2_MSCHAPV2) {
        return false; // PEAP-MSCHAPv2 only
    }
    return wifi_eap_text_valid(e->identity, 0, WIFI_EAP_IDENTITY_MAX) &&
           wifi_eap_text_valid(e->username, 1, WIFI_EAP_USER_MAX) &&
           wifi_eap_text_valid(e->password, 1, WIFI_EAP_PASS_MAX) && wifi_eap_ca_path_valid(e->ca_path);
}

bool wifi_eap_unverified(wifi_eap_t const *e) {
    return !e || e->ca_path[0] == '\0';
}

static size_t put(char *out, size_t at, char const *s) {
    size_t n = strlen(s);
    memcpy(out + at, s, n);
    out[at + n] = US;
    return at + n + 1;
}

size_t wifi_eap_encode(wifi_eap_t const *e, char *out, size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!wifi_eap_valid(e)) {
        return 0;
    }
    size_t need = 7 + strlen(e->identity) + 1 + strlen(e->username) + 1 + strlen(e->password) + 1 +
                  strlen(e->ca_path);
    if (need + 1 > cap) {
        return 0;
    }
    char head[7] = {VER0, VER1, US, (char)('0' + e->method), US, (char)('0' + e->phase2), US};
    memcpy(out, head, sizeof(head));
    size_t at = sizeof(head);
    at        = put(out, at, e->identity);
    at        = put(out, at, e->username);
    at        = put(out, at, e->password);
    strcpy(out + at, e->ca_path);
    return at + strlen(e->ca_path);
}

// Copies the next US-terminated field (or the rest, for the last one) into dst; NULL when it does not fit
static char const *take(char const *p, char *dst, size_t cap, bool last) {
    char const *end = strchr(p, US);
    size_t      len = end ? (size_t)(end - p) : strlen(p);
    if ((last && end) || (!last && !end) || len >= cap) {
        return NULL;
    }
    memcpy(dst, p, len);
    dst[len] = '\0';
    return p + len + (end ? 1 : 0);
}

bool wifi_eap_decode(char const *rec, wifi_eap_t *e) {
    if (!e) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    if (!rec || rec[0] != VER0 || rec[1] != VER1 || rec[2] != US || rec[4] != US || rec[6] != US) {
        return false;
    }
    if (rec[3] < '0' || rec[3] > '1' || rec[5] < '0' || rec[5] > '1') {
        return false;
    }
    e->method = (uint8_t)(rec[3] - '0');
    e->phase2 = (uint8_t)(rec[5] - '0');
    char const *p = rec + 7;
    p = take(p, e->identity, sizeof(e->identity), false);
    p = p ? take(p, e->username, sizeof(e->username), false) : NULL;
    p = p ? take(p, e->password, sizeof(e->password), false) : NULL;
    p = p ? take(p, e->ca_path, sizeof(e->ca_path), true) : NULL;
    if (!p || !wifi_eap_valid(e)) {
        wifi_eap_wipe(e);
        return false;
    }
    return true;
}

void wifi_eap_wipe(wifi_eap_t *e) {
    // volatile write loop: the compiler may not drop the clearing of a password copy that is about to go out of scope
    volatile unsigned char *p = (volatile unsigned char *)e;
    for (size_t i = 0; i < sizeof(*e); i++) {
        p[i] = 0;
    }
}
