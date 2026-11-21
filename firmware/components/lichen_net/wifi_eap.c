#include "wifi_eap.h"

#include <string.h>

#define US   '\x1f'
#define VER0 'E'
#define VER1 '2' // E1 (no server name) is still read

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

bool wifi_eap_server_valid(char const *name) {
    if (!name) {
        return false;
    }
    size_t n = 0;
    for (; name[n]; n++) {
        char c = name[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  c == '_';
        if (n >= WIFI_EAP_SERVER_MAX || !ok) {
            return false;
        }
    }
    return true;
}

bool wifi_eap_pap_without_ca(wifi_eap_t const *e) {
    return e && e->method == WIFI_EAP_TTLS && e->phase2 == WIFI_EAP_P2_PAP && e->ca_path[0] == '\0';
}

bool wifi_eap_pap_without_name(wifi_eap_t const *e) {
    return e && e->method == WIFI_EAP_TTLS && e->phase2 == WIFI_EAP_P2_PAP && e->ca_path[0] != '\0' &&
           e->server[0] == '\0';
}

wifi_eap_problem_t wifi_eap_problem(wifi_eap_t const *e) {
    if (!e || e->method > WIFI_EAP_TTLS || e->phase2 > WIFI_EAP_P2_PAP ||
        (e->method == WIFI_EAP_PEAP && e->phase2 != WIFI_EAP_P2_MSCHAPV2)) { // PEAP-MSCHAPv2 only
        return WIFI_EAP_BAD_METHOD;
    }
    if (!wifi_eap_text_valid(e->identity, 0, WIFI_EAP_IDENTITY_MAX)) {
        return WIFI_EAP_BAD_IDENTITY;
    }
    if (!wifi_eap_text_valid(e->username, 1, WIFI_EAP_USER_MAX)) {
        return WIFI_EAP_BAD_USER;
    }
    if (!wifi_eap_text_valid(e->password, 1, WIFI_EAP_PASS_MAX)) {
        return WIFI_EAP_BAD_PASSWORD;
    }
    if (!wifi_eap_ca_path_valid(e->ca_path)) {
        return WIFI_EAP_BAD_CA;
    }
    if (!wifi_eap_server_valid(e->server)) {
        return WIFI_EAP_BAD_SERVER;
    }
    // PAP sends the password in the clear inside the tunnel: only to a server that was checked, by CA and by name
    return wifi_eap_pap_without_ca(e) ? WIFI_EAP_PAP_NO_CA : wifi_eap_pap_without_name(e) ? WIFI_EAP_PAP_NO_NAME : WIFI_EAP_OK;
}

char const *wifi_eap_problem_text(wifi_eap_problem_t p) {
    switch (p) {
        case WIFI_EAP_BAD_METHOD: return "PEAP takes MSCHAPv2 only";
        case WIFI_EAP_BAD_IDENTITY: return "Identity: at most 63 characters, no control characters";
        case WIFI_EAP_BAD_USER: return "Username: 1 to 63 characters, no control characters";
        case WIFI_EAP_BAD_PASSWORD: return "Password: 1 to 127 characters";
        case WIFI_EAP_BAD_CA: return "CA file: /storage/<name>.pem or .crt";
        case WIFI_EAP_BAD_SERVER: return "Server name: letters, digits, dots and dashes only";
        case WIFI_EAP_PAP_NO_CA: return "PAP needs a CA certificate";
        case WIFI_EAP_PAP_NO_NAME: return "PAP needs a server name";
        default: return "";
    }
}

bool wifi_eap_valid(wifi_eap_t const *e) {
    return wifi_eap_problem(e) == WIFI_EAP_OK;
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
                  strlen(e->ca_path) + 1 + strlen(e->server);
    if (need + 1 > cap) {
        return 0;
    }
    char head[7] = {VER0, VER1, US, (char)('0' + e->method), US, (char)('0' + e->phase2), US};
    memcpy(out, head, sizeof(head));
    size_t at = sizeof(head);
    at        = put(out, at, e->identity);
    at        = put(out, at, e->username);
    at        = put(out, at, e->password);
    at        = put(out, at, e->ca_path);
    strcpy(out + at, e->server);
    return at + strlen(e->server);
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
    bool v1 = rec && rec[0] == VER0 && rec[1] == '1';
    if (!rec || rec[0] != VER0 || (rec[1] != VER1 && !v1) || rec[2] != US || rec[4] != US || rec[6] != US) {
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
    p = p ? take(p, e->ca_path, sizeof(e->ca_path), v1) : NULL; // E1 ends with the CA, E2 goes on with the name
    p = p && !v1 ? take(p, e->server, sizeof(e->server), true) : p;
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

wifi_eap_fail_t wifi_eap_config_fail(int err) {
    return err == 0 ? WIFI_EAP_FAIL_NONE : err == ERR_NO_MEM ? WIFI_EAP_FAIL_NOMEM : WIFI_EAP_FAIL_CONFIG;
}

char const *wifi_eap_fail_text(wifi_eap_fail_t why) {
    switch (why) {
        case WIFI_EAP_FAIL_C6_OLD: return "update the C6 (it has no 802.1X support)";
        case WIFI_EAP_FAIL_NOMEM: return "out of memory";
        case WIFI_EAP_FAIL_CA_FILE: return "the CA file is missing, too large or not PEM";
        case WIFI_EAP_FAIL_CONFIG: return "the C6 refused the Wi-Fi config";
        case WIFI_EAP_FAIL_CA_BAD: return "the C6 refused the CA certificate";
        case WIFI_EAP_FAIL_CRED: return "the saved credential is damaged, enter it again";
        default: return "";
    }
}

wifi_eap_ca_state_t wifi_eap_ca_state(wifi_eap_t const *e) {
    if (!e) {
        return WIFI_EAP_CA_NONE;
    }
    return wifi_eap_unverified(e) ? WIFI_EAP_CA_UNVERIFIED : e->server[0] ? WIFI_EAP_CA_VERIFIED : WIFI_EAP_CA_CA_ONLY;
}

unsigned wifi_eap_ca_reg(wifi_eap_ca_state_t s) {
    return s == WIFI_EAP_CA_VERIFIED ? 1u : s == WIFI_EAP_CA_CA_ONLY ? 2u : 0u;
}

wifi_name_plan_t wifi_eap_name_plan(bool has_name, bool name_on_c6) {
    return has_name ? WIFI_NAME_SET : name_on_c6 ? WIFI_NAME_DISABLE_FIRST : WIFI_NAME_NONE;
}

bool wifi_auth_mode_is_enterprise(int authmode) {
    wifi_auth_class_t c = wifi_auth_class(authmode, true);
    return c == WIFI_AUTH_CLASS_WPA_ENT || c == WIFI_AUTH_CLASS_WPA2_ENT || c == WIFI_AUTH_CLASS_WPA3_ENT;
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

unsigned wifi_apply_unwind(bool touched, bool have_prev, bool cfg_set) {
    return (touched ? WIFI_UNWIND_CLEAR : 0u) | (have_prev && cfg_set ? WIFI_UNWIND_RESTORE : 0u);
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

// A word or "-" (none) into an optional field
static bool copy_opt(char const *w, size_t n, char *out, size_t cap) {
    return word_is(w, n, "-") || copy_word(w, n, out, cap);
}

// The seven words and the password line into ssid and e; false when one does not fit
static bool fill_fields(char const *const w[7], size_t const n[7], char const *pass, char ssid[33], wifi_eap_t *e) {
    bool ok = copy_word(w[0], n[0], ssid, 33) && copy_opt(w[3], n[3], e->identity, sizeof(e->identity)) &&
              copy_word(w[4], n[4], e->username, sizeof(e->username)) && copy_opt(w[5], n[5], e->ca_path, sizeof(e->ca_path)) &&
              copy_opt(w[6], n[6], e->server, sizeof(e->server)) && strlen(pass) < sizeof(e->password);
    if (ok) {
        strcpy(e->password, pass);
    }
    return ok;
}

wifi_eap_parse_t wifi_eap_parse_line(char const *args, char ssid[33], wifi_eap_t *e) {
    memset(e, 0, sizeof(*e));
    ssid[0] = '\0';
    char const *p = args, *w[7];
    size_t      n[7];
    for (int i = 0; i < 7; i++) {
        if (!next_word(&p, &w[i], &n[i])) {
            return WIFI_EAP_PARSE_USAGE;
        }
    }
    if (*p == ' ') {
        p++; // one space separates the password from the last word; the password is the rest, spaces and all
    }
    bool peap = word_is(w[1], n[1], "peap"), ttls = word_is(w[1], n[1], "ttls");
    bool mschap = word_is(w[2], n[2], "mschapv2"), pap = word_is(w[2], n[2], "pap");
    if ((!peap && !ttls) || (!mschap && !pap) || !*p) {
        return WIFI_EAP_PARSE_USAGE;
    }
    e->method = ttls ? WIFI_EAP_TTLS : WIFI_EAP_PEAP;
    e->phase2 = pap ? WIFI_EAP_P2_PAP : WIFI_EAP_P2_MSCHAPV2;
    bool ok   = fill_fields(w, n, p, ssid, e);
    wifi_eap_problem_t why = ok ? wifi_eap_problem(e) : WIFI_EAP_BAD_METHOD;
    wifi_eap_parse_t   r   = why == WIFI_EAP_OK ? WIFI_EAP_PARSE_OK
                           : why == WIFI_EAP_PAP_NO_CA ? WIFI_EAP_PARSE_PAP_NO_CA
                           : why == WIFI_EAP_PAP_NO_NAME ? WIFI_EAP_PARSE_PAP_NO_NAME : WIFI_EAP_PARSE_INVALID;
    if (ssid[0] == '\0') {
        r = WIFI_EAP_PARSE_INVALID;
    }
    if (r != WIFI_EAP_PARSE_OK) {
        wifi_eap_wipe(e);
    }
    return r;
}
