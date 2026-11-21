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
    CHECK(!wifi_eap_valid(&b) && wifi_eap_pap_without_ca(&b)); // TTLS-PAP without a CA: refused
    strcpy(b.ca_path, "/storage/ca.pem");
    CHECK(wifi_eap_valid(&b) && !wifi_eap_pap_without_ca(&b)); // TTLS-PAP with a CA
    b.ca_path[0] = '\0';
    b.phase2 = WIFI_EAP_P2_MSCHAPV2;
    CHECK(wifi_eap_valid(&b)); // TTLS-MSCHAPv2 without a CA stays allowed (unverified warning)
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

// The kind key's mapping and the load rules (#496 step 2)
static void kind_persistence(void) {
    wifi_saved_t l = {0};
    CHECK(wifi_saved_add_eap(&l, "camp") && wifi_saved_add(&l, "home", "password1") && wifi_saved_add_eap(&l, "hall"));
    // list: hall, home, camp; two Enterprise entries never share a record slot
    CHECK(l.e[0].kind == WIFI_SAVED_EAP && l.e[2].kind == WIFI_SAVED_EAP && l.e[0].rec != l.e[2].rec);
    CHECK(wifi_saved_kind_code(&l.e[1]) == 0); // PSK: no key
    CHECK(wifi_saved_kind_code(&l.e[0]) == 1 + l.e[0].rec && wifi_saved_rec_mask(&l) == ((1u << l.e[0].rec) | (1u << l.e[2].rec)));
    CHECK(wifi_saved_eap_rec(&l, "camp") == l.e[2].rec && wifi_saved_eap_rec(&l, "home") == -1 && wifi_saved_eap_rec(&l, "x") == -1);

    wifi_saved_entry_t e;
    uint8_t            used = 0;
    memset(&e, 0, sizeof(e));
    strcpy(e.ssid, "net");
    strcpy(e.pass, "password1");
    CHECK(wifi_saved_slot_parse(&e, true, 0, &used) && e.kind == WIFI_SAVED_PSK && used == 0);
    CHECK(!wifi_saved_slot_parse(&e, false, 0, &used)); // no password key and no kind: not a slot
    strcpy(e.pass, "short");
    CHECK(!wifi_saved_slot_parse(&e, true, 0, &used)); // invalid PSK
    strcpy(e.pass, WIFI_SAVED_EAP_MARK);
    CHECK(!wifi_saved_slot_parse(&e, true, 0, &used)); // the marker without its kind key: dropped, never open
    CHECK(wifi_saved_slot_parse(&e, true, 3, &used) && e.kind == WIFI_SAVED_EAP && e.rec == 2 && e.pass[0] == '\0' && used == 4);
    CHECK(!wifi_saved_slot_parse(&e, true, 3, &used)); // the record slot is taken
    CHECK(wifi_saved_slot_parse(&e, false, 1, &used) && e.rec == 0 && used == 5); // an Enterprise slot needs no password
    CHECK(!wifi_saved_slot_parse(&e, false, WIFI_SAVED_MAX + 1, &used) && !wifi_saved_slot_parse(&e, false, 200, &used));
    e.ssid[0] = '\0';
    CHECK(!wifi_saved_slot_parse(&e, false, 5, &used));

    // promote moves an entry whole
    CHECK(wifi_saved_promote(&l, 2) && strcmp(l.e[0].ssid, "camp") == 0 && l.e[0].kind == WIFI_SAVED_EAP &&
          strcmp(l.e[1].ssid, "hall") == 0 && !wifi_saved_promote(&l, 3) && !wifi_saved_promote(&l, -1));
    // the slot of a replaced entry is free for the new one
    CHECK(wifi_saved_add_eap(&l, "camp") && wifi_saved_is_eap(&l, 0) && l.e[0].rec != l.e[1].rec);
}

// What the join says and shows (#496 step 2)
static void join_mapping(void) {
    CHECK(wifi_eap_rpc_fail(0, WIFI_EAP_STEP_OTHER) == WIFI_EAP_FAIL_NONE);
    CHECK(wifi_eap_rpc_fail(-1, WIFI_EAP_STEP_OTHER) == WIFI_EAP_FAIL_C6_OLD); // ESP_FAIL: an RPC the C6 does not know
    CHECK(wifi_eap_rpc_fail(0x103, WIFI_EAP_STEP_OTHER) == WIFI_EAP_FAIL_C6_OLD);
    CHECK(wifi_eap_rpc_fail(0x101, WIFI_EAP_STEP_OTHER) == WIFI_EAP_FAIL_NOMEM);
    CHECK(wifi_eap_rpc_fail(0x102, WIFI_EAP_STEP_CA) == WIFI_EAP_FAIL_CA_BAD);
    CHECK(wifi_eap_rpc_fail(0x102, WIFI_EAP_STEP_OTHER) == WIFI_EAP_FAIL_C6_OLD);
    CHECK(strstr(wifi_eap_fail_text(WIFI_EAP_FAIL_C6_OLD), "update the C6") != NULL);
    CHECK(wifi_eap_fail_text(WIFI_EAP_FAIL_NONE)[0] == '\0');
    // A refused set_config is its own reason, not "update the C6"
    CHECK(wifi_eap_config_fail(0) == WIFI_EAP_FAIL_NONE && wifi_eap_config_fail(-1) == WIFI_EAP_FAIL_CONFIG);
    CHECK(wifi_eap_config_fail(0x103) == WIFI_EAP_FAIL_CONFIG && wifi_eap_config_fail(0x101) == WIFI_EAP_FAIL_NOMEM);
    CHECK(strstr(wifi_eap_fail_text(WIFI_EAP_FAIL_CONFIG), "refused") != NULL &&
          strstr(wifi_eap_fail_text(WIFI_EAP_FAIL_CONFIG), "update the C6") == NULL);

    wifi_eap_t e = good();
    CHECK(wifi_eap_ca_state(NULL) == WIFI_EAP_CA_NONE && wifi_eap_ca_state(&e) == WIFI_EAP_CA_UNVERIFIED);
    strcpy(e.ca_path, "/storage/ca.pem");
    CHECK(wifi_eap_ca_state(&e) == WIFI_EAP_CA_VERIFIED);

    char const pem[] = "junk\n-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
    CHECK(wifi_eap_pem_ok(pem, sizeof(pem) - 1) && !wifi_eap_pem_ok(pem, 10) && !wifi_eap_pem_ok(NULL, 100));
    char const der[] = "\x30\x82\x01\x0a binary DER of a certificate, no armour";
    CHECK(!wifi_eap_pem_ok(der, sizeof(der) - 1));
    static char big[WIFI_EAP_CA_FILE_MAX + 2];
    memset(big, 'a', sizeof(big));
    memcpy(big, pem, sizeof(pem) - 1);
    CHECK(wifi_eap_pem_ok(big, WIFI_EAP_CA_FILE_MAX) && !wifi_eap_pem_ok(big, WIFI_EAP_CA_FILE_MAX + 1)); // larger: refused

    CHECK(wifi_auth_class(0, false) == WIFI_AUTH_CLASS_NONE && wifi_auth_class(0, true) == WIFI_AUTH_CLASS_OPEN);
    CHECK(wifi_auth_class(3, true) == WIFI_AUTH_CLASS_WPA2 && wifi_auth_class(5, true) == WIFI_AUTH_CLASS_WPA2_ENT);
    CHECK(wifi_auth_class(6, true) == WIFI_AUTH_CLASS_WPA3 && wifi_auth_class(14, true) == WIFI_AUTH_CLASS_WPA3_ENT);
    CHECK(wifi_auth_class(16, true) == WIFI_AUTH_CLASS_WPA_ENT && wifi_auth_class(99, true) == WIFI_AUTH_CLASS_OTHER);
}

// The apply decision (L4): PSK after Enterprise clears, an old C6 is never sent the clears, a blocked pair fails closed
static void apply_plan(void) {
    CHECK(wifi_apply_plan(false, false, false, false) == WIFI_PLAN_PSK);
    CHECK(wifi_apply_plan(false, false, false, true) == WIFI_PLAN_PSK_CLEAR); // PSK after an Enterprise network
    CHECK(wifi_apply_plan(true, true, false, false) == WIFI_PLAN_EAP && wifi_apply_plan(true, true, false, true) == WIFI_PLAN_EAP);
    CHECK(wifi_apply_plan(true, false, false, true) == WIFI_PLAN_FAIL); // record missing or damaged: blocked, never PSK
    CHECK(wifi_apply_plan(true, false, true, false) == WIFI_PLAN_FAIL);
    CHECK(wifi_apply_plan(false, false, true, true) == WIFI_PLAN_FAIL); // the marker with no Enterprise entry: not a password
    // An old C6: the first EAP call fails, nothing was touched, so the next PSK apply sends no clear
    bool on = wifi_enterprise_after(false, false);
    CHECK(!on && wifi_apply_plan(false, false, false, on) == WIFI_PLAN_PSK);
    // A new C6 where a later call failed: touched, so the PSK network that follows clears
    on = wifi_enterprise_after(false, true);
    CHECK(on && wifi_apply_plan(false, false, false, on) == WIFI_PLAN_PSK_CLEAR);
    CHECK(wifi_enterprise_after(true, false) && !wifi_enterprise_after(false, false));
    // What a refused Enterprise apply undoes: an old C6 (nothing touched) gets no clear; restore only after a set_config
    CHECK(wifi_apply_unwind(false, true, false) == 0 && wifi_apply_unwind(false, true, true) == WIFI_UNWIND_RESTORE);
    CHECK(wifi_apply_unwind(true, true, false) == WIFI_UNWIND_CLEAR);
    CHECK(wifi_apply_unwind(true, true, true) == (WIFI_UNWIND_CLEAR | WIFI_UNWIND_RESTORE) && wifi_apply_unwind(true, false, true) == WIFI_UNWIND_CLEAR);
}

// `fw wifi eap ...`
static void console_line(void) {
    char       ssid[33];
    wifi_eap_t e;
    CHECK(wifi_eap_parse_line("camp ttls pap anon@camp.example badge42 /storage/ca.pem pass word  x", ssid, &e) == WIFI_EAP_PARSE_OK);
    CHECK(strcmp(ssid, "camp") == 0 && e.method == WIFI_EAP_TTLS && e.phase2 == WIFI_EAP_P2_PAP &&
          strcmp(e.identity, "anon@camp.example") == 0 && strcmp(e.username, "badge42") == 0 &&
          strcmp(e.ca_path, "/storage/ca.pem") == 0 && strcmp(e.password, "pass word  x") == 0); // the rest, spaces and all
    CHECK(wifi_eap_parse_line("camp  peap  mschapv2 - user - secret", ssid, &e) == WIFI_EAP_PARSE_OK && e.identity[0] == '\0' &&
          e.ca_path[0] == '\0' && wifi_eap_unverified(&e) && strcmp(e.password, "secret") == 0);
    CHECK(wifi_eap_parse_line("camp ttls pap - user - secret", ssid, &e) == WIFI_EAP_PARSE_PAP_NO_CA && e.password[0] == '\0');
    CHECK(wifi_eap_parse_line("camp ttls mschapv2 - user - secret", ssid, &e) == WIFI_EAP_PARSE_OK);
    // A record that breaks the rule (hand-made: the encoder refuses it) does not decode, so the join fails cleanly
    char const bad[] = "E1\x1f" "1\x1f" "1\x1f" "\x1f" "user\x1fsecret\x1f";
    CHECK(!wifi_eap_decode(bad, &e));
    char withca[sizeof(bad) + 20];
    snprintf(withca, sizeof(withca), "%s/storage/ca.pem", bad);
    CHECK(wifi_eap_decode(withca, &e)); // the same record with a CA is fine: only the rule rejected it
    CHECK(wifi_eap_parse_line("camp peap pap - user - secret", ssid, &e) == WIFI_EAP_PARSE_INVALID); // PEAP is MSCHAPv2
    CHECK(wifi_eap_parse_line("camp peap mschapv2 - user - ", ssid, &e) == WIFI_EAP_PARSE_USAGE); // no password
    CHECK(wifi_eap_parse_line("camp peap mschapv2 - user", ssid, &e) == WIFI_EAP_PARSE_USAGE);
    CHECK(wifi_eap_parse_line("", ssid, &e) == WIFI_EAP_PARSE_USAGE);
    CHECK(wifi_eap_parse_line("camp tls mschapv2 - user - secret", ssid, &e) == WIFI_EAP_PARSE_USAGE); // no EAP-TLS
    CHECK(wifi_eap_parse_line("camp peap chap - user - secret", ssid, &e) == WIFI_EAP_PARSE_USAGE);
    CHECK(wifi_eap_parse_line("camp peap mschapv2 - user /etc/ca.pem secret", ssid, &e) == WIFI_EAP_PARSE_INVALID);
    CHECK(strlen(e.password) == 0 && e.username[0] == '\0'); // an invalid line leaves nothing behind
    char longssid[80] = "";
    memset(longssid, 'a', 40);
    strcpy(longssid + 40, " peap mschapv2 - user - secret");
    CHECK(wifi_eap_parse_line(longssid, ssid, &e) == WIFI_EAP_PARSE_INVALID);
    char big[400] = "camp peap mschapv2 - user - ";
    memset(big + strlen(big), 'p', 200);
    CHECK(wifi_eap_parse_line(big, ssid, &e) == WIFI_EAP_PARSE_INVALID); // password over 127 bytes
}

int main(void) {
    kind_persistence();
    join_mapping();
    apply_plan();
    console_line();
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
