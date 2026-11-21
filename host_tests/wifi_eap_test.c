// Host test: the WPA2/WPA3-Enterprise credential model (#496): validation, the stored record and the saved-list flag
#include "wifi_eap.h"
#include "wifi_saved.h"

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

static wifi_eap_t good(void) {
    wifi_eap_t e = {.method = WIFI_EAP_PEAP, .phase2 = WIFI_EAP_P2_MSCHAPV2};
    strcpy(e.identity, "anonymous@camp.example");
    strcpy(e.username, "badge42");
    strcpy(e.password, "p;ss w0rd\xc3\xa9"); // separators and UTF-8 in a password are fine
    return e;
}

static void validation(void) {
    wifi_eap_t e = good();
    CHECK(wifi_eap_valid(&e));
    CHECK(wifi_eap_unverified(&e)); // no CA: the UI warns
    strcpy(e.ca_path, "/storage/ca/camp.pem");
    CHECK(wifi_eap_valid(&e) && !wifi_eap_unverified(&e));

    wifi_eap_t b = good();
    b.identity[0] = '\0';
    CHECK(wifi_eap_valid(&b)); // empty outer identity: the join uses the username
    b.username[0] = '\0';
    CHECK(!wifi_eap_valid(&b));
    b = good();
    b.password[0] = '\0';
    CHECK(!wifi_eap_valid(&b));
    b = good();
    strcpy(b.username, "bad\nname");
    CHECK(!wifi_eap_valid(&b));
    b = good();
    b.password[3] = '\x1f'; // the record's own separator
    CHECK(!wifi_eap_valid(&b));
    b = good();
    memset(b.username, 'u', WIFI_EAP_USER_MAX);
    CHECK(wifi_eap_valid(&b));
    b.username[WIFI_EAP_USER_MAX] = 'u'; // no NUL left: not a string any more
    CHECK(!wifi_eap_valid(&b));
    b = good(); // UTF-8 at the length limit: the limit is bytes, 21 x 3 bytes fit, 22 do not
    for (int i = 0; i < 21; i++) {
        memcpy(b.username + 3 * i, "\xe2\x82\xac", 3);
    }
    b.username[63] = '\0';
    CHECK(wifi_eap_valid(&b));
    char rec[WIFI_EAP_BLOB_MAX];
    wifi_eap_t d;
    CHECK(wifi_eap_encode(&b, rec, sizeof(rec)) > 0 && wifi_eap_decode(rec, &d) && strcmp(d.username, b.username) == 0);
    b = good();
    b.method = WIFI_EAP_PEAP;
    b.phase2 = WIFI_EAP_P2_PAP; // PEAP-MSCHAPv2 only
    CHECK(!wifi_eap_valid(&b));
    b.method = WIFI_EAP_TTLS;
    CHECK(wifi_eap_valid(&b)); // TTLS-PAP
    b.phase2 = WIFI_EAP_P2_MSCHAPV2;
    CHECK(wifi_eap_valid(&b));
    b.method = 2;
    CHECK(!wifi_eap_valid(&b));
    CHECK(!wifi_eap_valid(NULL));
}

static void ca_paths(void) {
    CHECK(wifi_eap_ca_path_valid(""));
    CHECK(!wifi_eap_ca_path_valid(NULL));
    CHECK(wifi_eap_ca_path_valid("/storage/ca.pem") && wifi_eap_ca_path_valid("/storage/a/b/ca.crt"));
    CHECK(!wifi_eap_ca_path_valid("/storage/"));
    CHECK(!wifi_eap_ca_path_valid("ca.pem") && !wifi_eap_ca_path_valid("/sdcard/ca.pem"));
    CHECK(!wifi_eap_ca_path_valid("/storagex/ca.pem") && !wifi_eap_ca_path_valid("/storage/ca.txt"));
    CHECK(!wifi_eap_ca_path_valid("/storage/../etc/ca.pem") && !wifi_eap_ca_path_valid("/storage/a/../ca.pem"));
    CHECK(!wifi_eap_ca_path_valid("/storage/./ca.pem") && !wifi_eap_ca_path_valid("/storage//ca.pem"));
    CHECK(!wifi_eap_ca_path_valid("/storage/ca\n.pem"));
    CHECK(!wifi_eap_ca_path_valid("/storage/a\\..\\ca.pem") && !wifi_eap_ca_path_valid("/storage/a\\b.pem"));
    char longp[200] = "/storage/";
    memset(longp + 9, 'a', 150);
    strcpy(longp + 159, ".pem");
    CHECK(!wifi_eap_ca_path_valid(longp));
}

static void record(void) {
    char       rec[WIFI_EAP_BLOB_MAX];
    wifi_eap_t e = good();
    strcpy(e.ca_path, "/storage/ca/camp.pem");
    e.method = WIFI_EAP_TTLS;
    e.phase2 = WIFI_EAP_P2_PAP;
    size_t n = wifi_eap_encode(&e, rec, sizeof(rec));
    CHECK(n == strlen(rec) && n > 0);
    CHECK(strncmp(rec, "E1\x1f" "1\x1f" "1\x1f", 7) == 0); // version, method, phase2
    wifi_eap_t d;
    CHECK(wifi_eap_decode(rec, &d) && memcmp(&d, &e, sizeof(e)) == 0);

    // A cap one byte short refuses and leaves an empty string
    CHECK(wifi_eap_encode(&e, rec, n) == 0 && rec[0] == '\0');
    CHECK(wifi_eap_encode(&e, rec, n + 1) == n);

    // The largest credential still fits WIFI_EAP_BLOB_MAX
    wifi_eap_t m = {.method = WIFI_EAP_TTLS, .phase2 = WIFI_EAP_P2_PAP};
    memset(m.identity, 'i', WIFI_EAP_IDENTITY_MAX);
    memset(m.username, 'u', WIFI_EAP_USER_MAX);
    memset(m.password, 'p', WIFI_EAP_PASS_MAX);
    strcpy(m.ca_path, "/storage/");
    memset(m.ca_path + 9, 'c', WIFI_EAP_CA_MAX - 9 - 4);
    strcpy(m.ca_path + WIFI_EAP_CA_MAX - 4, ".pem");
    CHECK(wifi_eap_valid(&m));
    CHECK(wifi_eap_encode(&m, rec, sizeof(rec)) == sizeof(rec) - 1);
    CHECK(wifi_eap_decode(rec, &d) && memcmp(&d, &m, sizeof(m)) == 0);

    // Empty identity and CA survive the round trip
    e = good();
    e.identity[0] = '\0';
    CHECK(wifi_eap_encode(&e, rec, sizeof(rec)) > 0 && wifi_eap_decode(rec, &d) && d.identity[0] == '\0' &&
          d.ca_path[0] == '\0');

    // An invalid credential is never encoded
    e.username[0] = '\0';
    CHECK(wifi_eap_encode(&e, rec, sizeof(rec)) == 0 && rec[0] == '\0');
}

static void decode_rejects(void) {
    wifi_eap_t d;
    char const *const bad[] = {
        "",
        "E2\x1f" "0\x1f" "0\x1f" "id\x1f" "user\x1f" "pw\x1f",           // another version
        "E1\x1f" "0\x1f" "0\x1f" "id\x1f" "user\x1f" "pw",               // a field missing
        "E1\x1f" "0\x1f" "0\x1f" "id\x1f" "user\x1f" "pw\x1f" "\x1f",    // one field too many
        "E1\x1f" "2\x1f" "0\x1f" "id\x1f" "user\x1f" "pw\x1f",           // unknown method
        "E1\x1f" "0\x1f" "1\x1f" "id\x1f" "user\x1f" "pw\x1f",           // PEAP with PAP
        "E1\x1f" "0\x1f" "0\x1f" "id\x1f" "\x1f" "pw\x1f",               // empty username
        "E1\x1f" "0\x1f" "0\x1f" "id\x1f" "user\x1f" "\x1f",             // empty password
        "E1\x1f" "0\x1f" "0\x1f" "id\x1f" "user\x1f" "pw\x1f" "/etc/x",  // CA outside /storage
        "E1\x1f" "0\x1f" "0\x1f" "id\x1f" "us\ner\x1f" "pw\x1f",         // control byte
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(&d, 0x55, sizeof(d));
        CHECK(!wifi_eap_decode(bad[i], &d));
        wifi_eap_t zero = {0};
        CHECK(memcmp(&d, &zero, sizeof(d)) == 0); // a failed decode leaves no half-read password
    }
    CHECK(!wifi_eap_decode(NULL, &d));
    CHECK(!wifi_eap_decode(bad[1], NULL)); // no output struct: refused, not dereferenced
    // A field longer than its buffer is refused, not truncated
    char big[400] = "E1\x1f" "0\x1f" "0\x1f";
    memset(big + 7, 'a', 64);
    strcpy(big + 71, "\x1f" "user\x1f" "pw\x1f");
    CHECK(!wifi_eap_decode(big, &d));
}

static void wipe(void) {
    wifi_eap_t e = good();
    wifi_eap_wipe(&e);
    wifi_eap_t zero = {0};
    CHECK(memcmp(&e, &zero, sizeof(e)) == 0);
}

static wifi_ap_t ap(char const *ssid, int rssi, bool open) {
    wifi_ap_t a = {0};
    strcpy(a.ssid, ssid);
    a.rssi = (int8_t)rssi;
    a.open = open;
    return a;
}

static void saved_flag(void) {
    wifi_saved_t l = {0};
    CHECK(!wifi_saved_add_eap(&l, "") && l.n == 0);
    CHECK(wifi_saved_add(&l, "home", "password1") && wifi_saved_add_eap(&l, "camp") && l.n == 2);
    CHECK(wifi_saved_is_eap(&l, 0) && !wifi_saved_is_eap(&l, 1));
    CHECK(!wifi_saved_is_eap(&l, 2) && !wifi_saved_is_eap(&l, -1));
    CHECK(l.e[0].pass[0] == '\0'); // the credential is not in the list
    CHECK(wifi_saved_find(&l, "camp") == 0);

    // The auto-join picks an enterprise network when its AP is secured, never an open twin
    wifi_ap_t aps[] = {ap("camp", -60, false), ap("home", -50, false)};
    CHECK(wifi_saved_pick(&l, aps, 2, 0) == 1); // home is stronger
    CHECK(wifi_saved_pick(&l, aps, 2, 1u << 1) == 0); // skip home: camp is picked although its pass is empty
    aps[0].open = true;
    CHECK(wifi_saved_pick(&l, aps, 1, 0) == -1);

    // A PSK add of the same SSID replaces the enterprise entry (and the flag), and the other way round
    CHECK(wifi_saved_add(&l, "camp", "password2") && l.n == 2 && !wifi_saved_is_eap(&l, 0));
    CHECK(wifi_saved_add_eap(&l, "camp") && l.n == 2 && wifi_saved_is_eap(&l, 0) && l.e[0].pass[0] == '\0');
    // The oldest entry goes when the list is full, flag included
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        char name[8];
        snprintf(name, sizeof(name), "n%d", i);
        CHECK(wifi_saved_add(&l, name, "password1"));
    }
    CHECK(l.n == WIFI_SAVED_MAX && wifi_saved_find(&l, "camp") < 0);
    for (size_t i = 0; i < l.n; i++) {
        CHECK(!wifi_saved_is_eap(&l, (int)i));
    }
    CHECK(wifi_saved_forget(&l, "n4") && !wifi_saved_is_eap(&l, 0));
}

int main(void) {
    validation();
    ca_paths();
    record();
    decode_rejects();
    wipe();
    saved_flag();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("wifi_eap_test: ok\n");
    return 0;
}
