#include "wifi_eap_form.h"

#include <stdio.h>
#include <string.h>

void eap_form_init(lichen_sys_wifi_eap_t *e) {
    memset(e, 0, sizeof(*e));
    e->method = LICHEN_SYS_WIFI_EAP_PEAP;
    e->phase2 = LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2;
}

void eap_form_cycle_method(lichen_sys_wifi_eap_t *e) {
    if (e->method == LICHEN_SYS_WIFI_EAP_PEAP) {
        e->method = LICHEN_SYS_WIFI_EAP_TTLS;
    } else {
        e->method = LICHEN_SYS_WIFI_EAP_PEAP;
        e->phase2 = LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2;
    }
}

bool eap_form_cycle_inner(lichen_sys_wifi_eap_t *e) {
    if (e->method != LICHEN_SYS_WIFI_EAP_TTLS) {
        return false;
    }
    e->phase2 = e->phase2 == LICHEN_SYS_WIFI_EAP_P2_PAP ? LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2 : LICHEN_SYS_WIFI_EAP_P2_PAP;
    return true;
}

// The text, cut to EAP_VALUE_MAX bytes with "..." when longer; never inside a UTF-8 sequence
static void ellipsize(char const *s, char *out, size_t cap) {
    size_t n = strlen(s);
    if (n <= EAP_VALUE_MAX && n < cap) {
        memcpy(out, s, n + 1);
        return;
    }
    size_t keep = EAP_VALUE_MAX - 3;
    while (keep > 0 && ((unsigned char)s[keep] & 0xC0) == 0x80) {
        keep--; // s[keep] would be a continuation byte: the cut falls inside a character
    }
    memcpy(out, s, keep);
    strcpy(out + keep, "...");
}

void eap_form_value(lichen_sys_wifi_eap_t const *e, eap_field_t f, char *out, size_t cap) {
    if (cap < EAP_VALUE_MAX + 1) {
        if (cap) {
            out[0] = '\0';
        }
        return;
    }
    char const *text = NULL;
    switch (f) {
        case EAP_FIELD_METHOD: text = e->method == LICHEN_SYS_WIFI_EAP_TTLS ? "TTLS" : "PEAP"; break;
        case EAP_FIELD_INNER: text = e->phase2 == LICHEN_SYS_WIFI_EAP_P2_PAP ? "PAP" : "MSCHAPv2"; break;
        case EAP_FIELD_IDENTITY: text = e->identity; break;
        case EAP_FIELD_USER: text = e->username; break;
        case EAP_FIELD_PASSWORD: text = e->password[0] ? "Set" : ""; break; // write-only: never the text
        case EAP_FIELD_CA: {
            char const *slash = strrchr(e->ca_path, '/');
            text              = slash ? slash + 1 : e->ca_path;
            break;
        }
        case EAP_FIELD_SERVER: text = e->server; break;
        default: break;
    }
    if (!text || !text[0]) {
        snprintf(out, cap, "%s", f == EAP_FIELD_IDENTITY ? "Same as username" : "Not set"); // empty identity = the username goes out
        return;
    }
    ellipsize(text, out, cap);
}

static bool plain_text(char const *s, size_t min, size_t max) {
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (n >= max || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return n >= min;
}

size_t eap_form_field_max(eap_field_t f) {
    switch (f) {
        case EAP_FIELD_IDENTITY: return LICHEN_SYS_WIFI_EAP_IDENTITY_MAX;
        case EAP_FIELD_USER: return LICHEN_SYS_WIFI_EAP_USER_MAX;
        case EAP_FIELD_PASSWORD: return LICHEN_SYS_WIFI_EAP_PASS_MAX;
        case EAP_FIELD_CA: return LICHEN_SYS_WIFI_EAP_CA_MAX;
        case EAP_FIELD_SERVER: return LICHEN_SYS_WIFI_EAP_SERVER_MAX; // under the 128 text_entry takes
        default: return 0;
    }
}

char const *eap_form_field_name(eap_field_t f) {
    switch (f) {
        case EAP_FIELD_IDENTITY: return "Identity";
        case EAP_FIELD_USER: return "Username";
        case EAP_FIELD_PASSWORD: return "Password";
        case EAP_FIELD_CA: return "CA file";
        case EAP_FIELD_SERVER: return "Server name";
        default: return "";
    }
}

char const *eap_form_field_hint(eap_field_t f) {
    switch (f) {
        case EAP_FIELD_IDENTITY: return "(sent in clear, empty: the username)";
        case EAP_FIELD_CA: return "(/storage/<name>.pem, empty: none)";
        case EAP_FIELD_SERVER: return "(empty: any name passes)";
        default: return "";
    }
}

char const *eap_form_field_error(eap_field_t f, char const *text) {
    if (!text) {
        return "Nothing entered";
    }
    switch (f) {
        case EAP_FIELD_IDENTITY: return plain_text(text, 0, LICHEN_SYS_WIFI_EAP_IDENTITY_MAX) ? NULL : "Identity: at most 63 characters";
        case EAP_FIELD_USER: return plain_text(text, 1, LICHEN_SYS_WIFI_EAP_USER_MAX) ? NULL : "Username: 1 to 63 characters";
        case EAP_FIELD_PASSWORD: return plain_text(text, 1, LICHEN_SYS_WIFI_EAP_PASS_MAX) ? NULL : "Password: 1 to 127 characters";
        case EAP_FIELD_CA:
            return lichen_sys_wifi_eap_ca_path_valid(text) ? NULL : "CA file: /storage/<name>.pem or .crt";
        case EAP_FIELD_SERVER:
            return lichen_sys_wifi_eap_server_valid(text) ? NULL : "Server name: letters, digits, dots, dashes";
        default: return "Not a text field";
    }
}

bool eap_form_set(lichen_sys_wifi_eap_t *e, eap_field_t f, char const *text) {
    char  *dst = NULL;
    size_t cap = 0;
    switch (f) {
        case EAP_FIELD_IDENTITY: dst = e->identity; cap = sizeof(e->identity); break;
        case EAP_FIELD_USER: dst = e->username; cap = sizeof(e->username); break;
        case EAP_FIELD_PASSWORD: dst = e->password; cap = sizeof(e->password); break;
        case EAP_FIELD_CA: dst = e->ca_path; cap = sizeof(e->ca_path); break;
        case EAP_FIELD_SERVER: dst = e->server; cap = sizeof(e->server); break;
        default: return false;
    }
    snprintf(dst, cap, "%s", text);
    return true;
}

bool eap_form_check(lichen_sys_wifi_eap_t const *e, char *why, size_t cap) {
    lichen_sys_wifi_eap_problem_t p = lichen_sys_wifi_eap_problem(e);
    if (p == LICHEN_SYS_WIFI_EAP_OK) {
        if (cap) {
            why[0] = '\0';
        }
        return true;
    }
    snprintf(why, cap, "%s", lichen_sys_wifi_eap_problem_text(p));
    return false;
}

char const *eap_form_warning(lichen_sys_wifi_eap_t const *e) {
    if (e->ca_path[0] == '\0') {
        return "No CA file: the server is not verified, an impostor access point could take the login.";
    }
    if (e->server[0] == '\0') {
        return "No server name: any certificate that CA signed is accepted.";
    }
    return "";
}
