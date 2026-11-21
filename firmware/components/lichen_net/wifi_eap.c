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

bool wifi_eap_pap_without_ca(wifi_eap_t const *e) {
    return e && e->method == WIFI_EAP_TTLS && e->phase2 == WIFI_EAP_P2_PAP && e->ca_path[0] == '\0';
}

bool wifi_eap_valid(wifi_eap_t const *e) {
    if (!e || e->method > WIFI_EAP_TTLS || e->phase2 > WIFI_EAP_P2_PAP) {
        return false;
    }
    if (e->method == WIFI_EAP_PEAP && e->phase2 != WIFI_EAP_P2_MSCHAPV2) {
        return false; // PEAP-MSCHAPv2 only
    }
    if (wifi_eap_pap_without_ca(e)) {
        return false; // PAP sends the password in the clear inside the tunnel: only to a server that was checked
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

// ---- joining (step 2) ----------------------------------------------------------------------------------------------

bool wifi_eap_pem_ok(char const *buf, size_t len) {
    static char const begin[] = "-----BEGIN CERTIFICATE-----";
    if (!buf || len < sizeof(begin) || len > WIFI_EAP_CA_FILE_MAX) {
        return false;
    }
    for (size_t i = 0; i + sizeof(begin) - 1 <= len; i++) {
        if (memcmp(buf + i, begin, sizeof(begin) - 1) == 0) {
            return true;
        }
    }
    return false;
}

#define ERR_NO_MEM      0x101 // ESP_ERR_NO_MEM, spelt out to stay ESP-free
#define ERR_INVALID_ARG 0x102 // ESP_ERR_INVALID_ARG

wifi_eap_fail_t wifi_eap_rpc_fail(int err, wifi_eap_step_t step) {
    if (err == 0) {
        return WIFI_EAP_FAIL_NONE;
    }
    if (err == ERR_NO_MEM) {
        return WIFI_EAP_FAIL_NOMEM;
    }
    if (step == WIFI_EAP_STEP_CA && err == ERR_INVALID_ARG) {
        return WIFI_EAP_FAIL_CA_BAD;
    }
    return WIFI_EAP_FAIL_C6_OLD;
}

char const *wifi_eap_fail_text(wifi_eap_fail_t why) {
    switch (why) {
        case WIFI_EAP_FAIL_C6_OLD: return "update the C6 (it has no 802.1X support)";
        case WIFI_EAP_FAIL_NOMEM: return "out of memory";
        case WIFI_EAP_FAIL_CA_FILE: return "the CA file is missing, too large or not PEM";
        case WIFI_EAP_FAIL_CA_BAD: return "the C6 refused the CA certificate";
        case WIFI_EAP_FAIL_CRED: return "the saved credential is damaged, enter it again";
        default: return "";
    }
}

wifi_eap_ca_state_t wifi_eap_ca_state(wifi_eap_t const *e) {
    return !e ? WIFI_EAP_CA_NONE : wifi_eap_unverified(e) ? WIFI_EAP_CA_UNVERIFIED : WIFI_EAP_CA_VERIFIED;
}

wifi_auth_class_t wifi_auth_class(int authmode, bool known) {
    if (!known) {
        return WIFI_AUTH_CLASS_NONE;
    }
    switch (authmode) { // wifi_auth_mode_t of ESP-IDF 6.1
        case 0: return WIFI_AUTH_CLASS_OPEN;
        case 1: return WIFI_AUTH_CLASS_WEP;
        case 2: return WIFI_AUTH_CLASS_WPA;
        case 3: return WIFI_AUTH_CLASS_WPA2;
        case 4: return WIFI_AUTH_CLASS_WPA_WPA2;
        case 5: return WIFI_AUTH_CLASS_WPA2_ENT;
        case 6: return WIFI_AUTH_CLASS_WPA3;
        case 7: return WIFI_AUTH_CLASS_WPA2_WPA3;
        case 10: case 14: case 15: return WIFI_AUTH_CLASS_WPA3_ENT;
        case 16: return WIFI_AUTH_CLASS_WPA_ENT;
        default: return WIFI_AUTH_CLASS_OTHER;
    }
}

wifi_plan_t wifi_apply_plan(bool is_eap_entry, bool record_ok, bool pass_is_mark, bool enterprise_on) {
    if (is_eap_entry) {
        return record_ok ? WIFI_PLAN_EAP : WIFI_PLAN_FAIL;
    }
    if (pass_is_mark) {
        return WIFI_PLAN_FAIL;
    }
    return enterprise_on ? WIFI_PLAN_PSK_CLEAR : WIFI_PLAN_PSK;
}

bool wifi_enterprise_after(bool was_on, bool touched) {
    return was_on || touched;
}

// The next space-separated word of *p (runs of spaces between words count as one); false at the end of the line
static bool next_word(char const **p, char const **w, size_t *n) {
    while (**p == ' ') {
        (*p)++;
    }
    if (!**p) {
        return false;
    }
    *w = *p;
    while (**p && **p != ' ') {
        (*p)++;
    }
    *n = (size_t)(*p - *w);
    return true;
}

static bool word_is(char const *w, size_t n, char const *s) {
    return strlen(s) == n && memcmp(w, s, n) == 0;
}

static bool copy_word(char const *w, size_t n, char *out, size_t cap) {
    if (n >= cap) {
        return false;
    }
    memcpy(out, w, n);
    out[n] = '\0';
    return true;
}

wifi_eap_parse_t wifi_eap_parse_line(char const *args, char ssid[33], wifi_eap_t *e) {
    memset(e, 0, sizeof(*e));
    ssid[0] = '\0';
    char const *p = args, *w[6];
    size_t      n[6];
    for (int i = 0; i < 6; i++) {
        if (!next_word(&p, &w[i], &n[i])) {
            return WIFI_EAP_PARSE_USAGE;
        }
    }
    if (*p == ' ') {
        p++; // one space separates the password from the CA path; the password is the rest, spaces and all
    }
    bool peap = word_is(w[1], n[1], "peap"), ttls = word_is(w[1], n[1], "ttls");
    bool mschap = word_is(w[2], n[2], "mschapv2"), pap = word_is(w[2], n[2], "pap");
    if ((!peap && !ttls) || (!mschap && !pap) || !*p) {
        return WIFI_EAP_PARSE_USAGE;
    }
    e->method = ttls ? WIFI_EAP_TTLS : WIFI_EAP_PEAP;
    e->phase2 = pap ? WIFI_EAP_P2_PAP : WIFI_EAP_P2_MSCHAPV2;
    bool ok   = copy_word(w[0], n[0], ssid, 33) && (word_is(w[3], n[3], "-") || copy_word(w[3], n[3], e->identity, sizeof(e->identity))) &&
              copy_word(w[4], n[4], e->username, sizeof(e->username)) &&
              (word_is(w[5], n[5], "-") || copy_word(w[5], n[5], e->ca_path, sizeof(e->ca_path))) &&
              strlen(p) < sizeof(e->password);
    if (ok) {
        strcpy(e->password, p);
    }
    if (ok && wifi_eap_pap_without_ca(e)) {
        wifi_eap_wipe(e);
        return WIFI_EAP_PARSE_PAP_NO_CA;
    }
    if (!ok || ssid[0] == '\0' || !wifi_eap_valid(e)) {
        wifi_eap_wipe(e);
        return WIFI_EAP_PARSE_INVALID;
    }
    return WIFI_EAP_PARSE_OK;
}
