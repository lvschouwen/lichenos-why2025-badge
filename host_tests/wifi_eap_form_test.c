// Host test: the Enterprise form of the Wi-Fi screen (#496): rows, field rules, refusals, the password never shown
#include "wifi_eap_form.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void methods(void) {
    lichen_sys_wifi_eap_t e;
    eap_form_init(&e);
    char v[EAP_VALUE_MAX + 1];
    eap_form_value(&e, EAP_FIELD_METHOD, v, sizeof(v));
    CHECK(strcmp(v, "PEAP") == 0);
    eap_form_value(&e, EAP_FIELD_INNER, v, sizeof(v));
    CHECK(strcmp(v, "MSCHAPv2") == 0);
    CHECK(!eap_form_cycle_inner(&e) && e.phase2 == LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2); // PEAP has no choice
    eap_form_cycle_method(&e);
    CHECK(e.method == LICHEN_SYS_WIFI_EAP_TTLS);
    CHECK(eap_form_cycle_inner(&e) && e.phase2 == LICHEN_SYS_WIFI_EAP_P2_PAP);
    eap_form_value(&e, EAP_FIELD_INNER, v, sizeof(v));
    CHECK(strcmp(v, "PAP") == 0);
    eap_form_cycle_method(&e); // back to PEAP: PAP does not survive
    CHECK(e.method == LICHEN_SYS_WIFI_EAP_PEAP && e.phase2 == LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2);
    eap_form_cycle_method(&e);
    CHECK(eap_form_cycle_inner(&e) && eap_form_cycle_inner(&e) && e.phase2 == LICHEN_SYS_WIFI_EAP_P2_MSCHAPV2);
}

static void rows(void) {
    lichen_sys_wifi_eap_t e;
    eap_form_init(&e);
    char v[EAP_VALUE_MAX + 1];
    eap_form_value(&e, EAP_FIELD_PASSWORD, v, sizeof(v));
    CHECK(strcmp(v, "Not set") == 0);
    eap_form_value(&e, EAP_FIELD_IDENTITY, v, sizeof(v));
    CHECK(strcmp(v, "Same as username") == 0);
    CHECK(eap_form_set(&e, EAP_FIELD_PASSWORD, "hunter2hunter2"));
    eap_form_value(&e, EAP_FIELD_PASSWORD, v, sizeof(v));
    CHECK(strcmp(v, "Set") == 0 && strstr(v, "hunter") == NULL); // write-only: the row never carries the text
    for (int f = 0; f < EAP_FIELD_COUNT; f++) {
        eap_form_value(&e, (eap_field_t)f, v, sizeof(v));
        CHECK(strstr(v, "hunter") == NULL);
    }
    // The CA row shows the file name, not the directory
    CHECK(eap_form_set(&e, EAP_FIELD_CA, "/storage/certs/camp-root.pem"));
    eap_form_value(&e, EAP_FIELD_CA, v, sizeof(v));
    CHECK(strcmp(v, "camp-root.pem") == 0);
    // A long value is cut with "..." and always fits; the cut never splits a UTF-8 character
    char longname[LICHEN_SYS_WIFI_EAP_USER_MAX + 1];
    memset(longname, 'u', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    CHECK(eap_form_set(&e, EAP_FIELD_USER, longname));
    eap_form_value(&e, EAP_FIELD_USER, v, sizeof(v));
    CHECK(strlen(v) == EAP_VALUE_MAX && strcmp(v + EAP_VALUE_MAX - 3, "...") == 0);
    for (size_t i = 0; i < 20; i++) { // euro signs: 3 bytes each, so the cut meets every offset within a character
        memset(longname, 'a', i);
        for (size_t k = 0; k < 10; k++) {
            memcpy(longname + i + 3 * k, "\xe2\x82\xac", 3);
        }
        longname[i + 30] = '\0';
        eap_form_set(&e, EAP_FIELD_IDENTITY, longname);
        eap_form_value(&e, EAP_FIELD_IDENTITY, v, sizeof(v));
        size_t n = strlen(v);
        CHECK(n <= EAP_VALUE_MAX && strcmp(v + n - 3, "...") == 0);
        for (size_t k = 0; k + 3 < n; k++) { // every non-ASCII byte starts or continues a whole euro sign
            if ((unsigned char)v[k] == 0xE2) {
                CHECK(k + 3 <= n - 3 && (unsigned char)v[k + 1] == 0x82 && (unsigned char)v[k + 2] == 0xAC);
            }
        }
    }
    // A cap below the row's width writes nothing rather than overrun
    char tiny[4] = "xyz";
    eap_form_value(&e, EAP_FIELD_USER, tiny, sizeof(tiny));
    CHECK(tiny[0] == '\0');
}

static void field_rules(void) {
    CHECK(eap_form_field_error(EAP_FIELD_USER, "badge") == NULL && eap_form_field_error(EAP_FIELD_USER, "") != NULL);
    CHECK(eap_form_field_error(EAP_FIELD_IDENTITY, "") == NULL); // an empty identity is allowed
    CHECK(eap_form_field_error(EAP_FIELD_PASSWORD, "") != NULL && eap_form_field_error(EAP_FIELD_PASSWORD, "pw") == NULL);
    CHECK(eap_form_field_error(EAP_FIELD_PASSWORD, "a\nb") != NULL); // no control bytes
    CHECK(eap_form_field_error(EAP_FIELD_CA, "") == NULL && eap_form_field_error(EAP_FIELD_CA, "/storage/ca.pem") == NULL);
    CHECK(eap_form_field_error(EAP_FIELD_CA, "/etc/ca.pem") != NULL && eap_form_field_error(EAP_FIELD_CA, "ca.pem") != NULL);
    CHECK(eap_form_field_error(EAP_FIELD_SERVER, "") == NULL && eap_form_field_error(EAP_FIELD_SERVER, "radius.camp.example") == NULL);
    CHECK(eap_form_field_error(EAP_FIELD_SERVER, "bad name") != NULL);
    CHECK(eap_form_field_error(EAP_FIELD_METHOD, "x") != NULL && eap_form_field_error(EAP_FIELD_USER, NULL) != NULL);
    // The refusals never carry the typed text
    CHECK(strstr(eap_form_field_error(EAP_FIELD_PASSWORD, "\nsecret"), "secret") == NULL);
    // The entry takes what the field holds and no more
    CHECK(eap_form_field_max(EAP_FIELD_PASSWORD) == 127 && eap_form_field_max(EAP_FIELD_USER) == 63 &&
          eap_form_field_max(EAP_FIELD_CA) == 95 && eap_form_field_max(EAP_FIELD_SERVER) == 127 &&
          eap_form_field_max(EAP_FIELD_IDENTITY) == 63);
    CHECK(eap_form_field_max(EAP_FIELD_PASSWORD) <= 128 && eap_form_field_max(EAP_FIELD_SERVER) <= 128); // text_entry's cap
    lichen_sys_wifi_eap_t e;
    eap_form_init(&e);
    CHECK(!eap_form_set(&e, EAP_FIELD_METHOD, "TTLS") && e.method == LICHEN_SYS_WIFI_EAP_PEAP);
    CHECK(strcmp(eap_form_field_name(EAP_FIELD_SERVER), "Server name") == 0 && eap_form_field_hint(EAP_FIELD_USER)[0] == '\0');
    CHECK(strstr(eap_form_field_hint(EAP_FIELD_IDENTITY), "clear") != NULL);
}

static void save_checks(void) {
    lichen_sys_wifi_eap_t e;
    char                  why[100];
    eap_form_init(&e);
    CHECK(!eap_form_check(&e, why, sizeof(why)) && strstr(why, "Username") != NULL); // nothing typed yet
    eap_form_set(&e, EAP_FIELD_USER, "badge");
    CHECK(!eap_form_check(&e, why, sizeof(why)) && strstr(why, "Password") != NULL);
    eap_form_set(&e, EAP_FIELD_PASSWORD, "pw");
    CHECK(eap_form_check(&e, why, sizeof(why)) && why[0] == '\0'); // PEAP-MSCHAPv2 with no CA: allowed, with a warning
    CHECK(strstr(eap_form_warning(&e), "not verified") != NULL);
    eap_form_cycle_method(&e);
    eap_form_cycle_inner(&e); // TTLS-PAP
    CHECK(!eap_form_check(&e, why, sizeof(why)) && strstr(why, "PAP needs a CA") != NULL);
    eap_form_set(&e, EAP_FIELD_CA, "/storage/ca.pem");
    CHECK(!eap_form_check(&e, why, sizeof(why)) && strstr(why, "PAP needs a server name") != NULL);
    CHECK(strstr(eap_form_warning(&e), "No server name") != NULL);
    eap_form_set(&e, EAP_FIELD_SERVER, "radius.camp.example");
    CHECK(eap_form_check(&e, why, sizeof(why)) && eap_form_warning(&e)[0] == '\0');
    eap_form_set(&e, EAP_FIELD_SERVER, "");
    CHECK(!eap_form_check(&e, why, 5) && strlen(why) == 4); // a short buffer is cut, never overrun
}

int main(void) {
    methods();
    rows();
    field_rules();
    save_checks();
    if (failures) {
        printf("wifi_eap_form_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("wifi_eap_form_test: all passed\n");
    return 0;
}
