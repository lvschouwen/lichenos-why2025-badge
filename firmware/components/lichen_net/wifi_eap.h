#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The WPA2/WPA3-Enterprise credential of one saved network (#496), ESP-free: the model, its validation and the text
// record it is stored as. PEAP-MSCHAPv2 and TTLS (MSCHAPv2 or PAP inner) only: no client certificates, no EAP-TLS.
// The password is write-only like the PSK: nothing here prints it, and wifi_eap_wipe clears a copy.

#define WIFI_EAP_IDENTITY_MAX 63 // the outer (anonymous) identity; empty = use the username
#define WIFI_EAP_USER_MAX     63
#define WIFI_EAP_PASS_MAX     127
#define WIFI_EAP_CA_MAX       95  // a path, not the PEM itself
#define WIFI_EAP_CA_DIR       "/storage/"
#define WIFI_EAP_SERVER_MAX   127 // the server name the certificate must carry (a host name)
// 7 header bytes ("E2", method, phase2 and their separators), 4 separators between the five fields, the fields, the NUL
#define WIFI_EAP_BLOB_MAX \
    (7 + 4 + WIFI_EAP_IDENTITY_MAX + WIFI_EAP_USER_MAX + WIFI_EAP_PASS_MAX + WIFI_EAP_CA_MAX + WIFI_EAP_SERVER_MAX + 1)

typedef enum { WIFI_EAP_PEAP = 0, WIFI_EAP_TTLS = 1 } wifi_eap_method_t;
typedef enum { WIFI_EAP_P2_MSCHAPV2 = 0, WIFI_EAP_P2_PAP = 1 } wifi_eap_phase2_t;

typedef struct {
    uint8_t method; // wifi_eap_method_t
    uint8_t phase2; // wifi_eap_phase2_t; PEAP is always MSCHAPV2
    char    identity[WIFI_EAP_IDENTITY_MAX + 1];
    char    username[WIFI_EAP_USER_MAX + 1];
    char    password[WIFI_EAP_PASS_MAX + 1];
    char    ca_path[WIFI_EAP_CA_MAX + 1]; // empty = no CA: connect unverified, and the UI says so
    char    server[WIFI_EAP_SERVER_MAX + 1]; // empty = no name check (record E1 has none); needs a CA to mean anything
} wifi_eap_t;

// A text field: no control bytes (< 0x20, 0x7F; the record's separator is one), UTF-8 bytes allowed, length in range
bool wifi_eap_text_valid(char const *s, size_t min, size_t max);
// Empty (no CA) or an absolute path under /storage/ ending in .pem or .crt, no empty, "." or ".." segment, no control
// bytes, at most WIFI_EAP_CA_MAX bytes. Whether the file exists is the join's business.
bool wifi_eap_ca_path_valid(char const *path);
// Empty (no name check) or 1-127 bytes of letters, digits, '.', '-' and '_': a host name, matched exactly (a wildcard
// certificate covers its names) by the C6's TLS. No spaces, no '*', no control bytes.
bool wifi_eap_server_valid(char const *name);
// TTLS with the PAP inner method and no CA: PAP puts the cleartext password in the tunnel, so an unverified server (an evil
// twin) would receive it. wifi_eap_valid refuses it; the UI and console say "PAP needs a CA certificate".
bool wifi_eap_pap_without_ca(wifi_eap_t const *e);
// TTLS-PAP with a CA but no server name: any certificate the CA signed would take the password (#496), so PAP needs the
// name as well. wifi_eap_valid refuses it ("PAP needs a server name").
bool wifi_eap_pap_without_name(wifi_eap_t const *e);
// The first thing wrong with a credential, in the order the fields are asked: what the screen and the console tell
typedef enum {
    WIFI_EAP_OK,
    WIFI_EAP_BAD_METHOD,   // an unknown method or inner method, or PEAP with PAP
    WIFI_EAP_BAD_IDENTITY, // longer than 63 bytes or holds a control byte
    WIFI_EAP_BAD_USER,     // empty, longer than 63 bytes or holds a control byte
    WIFI_EAP_BAD_PASSWORD, // empty, longer than 127 bytes or holds a control byte
    WIFI_EAP_BAD_CA,       // not an absolute /storage/ path ending in .pem or .crt
    WIFI_EAP_BAD_SERVER,   // not a host name
    WIFI_EAP_PAP_NO_CA,    // TTLS-PAP without a CA
    WIFI_EAP_PAP_NO_NAME,  // TTLS-PAP with a CA and no server name
} wifi_eap_problem_t;
wifi_eap_problem_t wifi_eap_problem(wifi_eap_t const *e);
// The refusal in plain words for the screen and the console (what to do is in it); "" for WIFI_EAP_OK
char const *wifi_eap_problem_text(wifi_eap_problem_t p);
// Method and inner method known and allowed together, username 1-63, identity 0-63, password 1-127, CA path valid, server
// name valid, and PAP only with a CA and a server name: wifi_eap_problem() == WIFI_EAP_OK
bool wifi_eap_valid(wifi_eap_t const *e);
// True when the join would not check the server: no CA set (the UI shows "unverified server")
bool wifi_eap_unverified(wifi_eap_t const *e);

// The record kept as one secret string (the password is in it): "E2" US method US phase2 US identity US username US
// password US ca_path US server with US = 0x1F and the digits '0'/'1'. Returns the length without the NUL, 0 for an
// invalid credential or a cap below the record's size. The leading "E2" is the version. A decode also takes "E1" (no
// server field, so an empty name, which a PAP credential cannot have: such a record fails closed and is entered again);
// any other version fails.
size_t wifi_eap_encode(wifi_eap_t const *e, char *out, size_t cap);
// Parses and validates a record; on any failure *e is zeroed and false is returned
bool   wifi_eap_decode(char const *rec, wifi_eap_t *e);
void   wifi_eap_wipe(wifi_eap_t *e);

// ---- joining (step 2) ----------------------------------------------------------------------------------------------

// The CA file is read whole and sent to the C6 in one RPC: a single root or a short chain. A larger file is refused (the
// join fails), it never goes out cut. The PEM is sent with its NUL (mbedTLS parses PEM only when the length counts it).
#define WIFI_EAP_CA_FILE_MAX 4096
// The CA text is PEM: a certificate block, not DER binary and not an empty or unrelated file
bool wifi_eap_pem_ok(char const *buf, size_t len);

// Why an Enterprise join did not start (never an authentication failure: those come from the C6 as disconnect reasons)
typedef enum {
    WIFI_EAP_FAIL_NONE,
    WIFI_EAP_FAIL_C6_OLD, // an esp_eap_client_* RPC failed: the C6 image has no 802.1X (or an older RPC set)
    WIFI_EAP_FAIL_NOMEM,  // the C6 or the P4 ran out of memory
    WIFI_EAP_FAIL_CA_FILE, // the CA path is set but the file is missing, unreadable, too large or not PEM
    WIFI_EAP_FAIL_CA_BAD, // the C6 refused the CA certificate
    WIFI_EAP_FAIL_CONFIG, // the C6 refused esp_wifi_set_config for the network
    WIFI_EAP_FAIL_CRED,   // the saved credential record is missing or damaged
} wifi_eap_fail_t;
typedef enum { WIFI_EAP_STEP_OTHER, WIFI_EAP_STEP_CA } wifi_eap_step_t;
// Maps the esp_err_t of one EAP RPC (0 = ESP_OK): out of memory stays out of memory, a refused CA is a bad CA, anything
// else (an RPC the C6 does not know answers with an error too) is "update the C6"
wifi_eap_fail_t wifi_eap_rpc_fail(int err, wifi_eap_step_t step);
// The same for esp_wifi_set_config: out of memory stays out of memory, any other refusal is the config, not an old C6
wifi_eap_fail_t wifi_eap_config_fail(int err);
// The reason as the console and the screen word it
char const *wifi_eap_fail_text(wifi_eap_fail_t why);

// What the applied configuration does about the server certificate. NONE: the driver is not set for an Enterprise network
// (e is NULL). VERIFIED: a CA and a server name. CA_ONLY: a CA alone, so any certificate that CA signed passes.
// UNVERIFIED: no CA at all (the join warns). The registry's wifi.eap_ca shows UNVERIFIED as none: see wifi_eap_ca_reg.
typedef enum {
    WIFI_EAP_CA_NONE, WIFI_EAP_CA_VERIFIED, WIFI_EAP_CA_UNVERIFIED, WIFI_EAP_CA_CA_ONLY
} wifi_eap_ca_state_t;
wifi_eap_ca_state_t wifi_eap_ca_state(wifi_eap_t const *e);
// The registry value of wifi.eap_ca (enum none, verified, ca_only): verified with a CA and a name, ca_only with a CA
// alone, none otherwise
unsigned wifi_eap_ca_reg(wifi_eap_ca_state_t s);

// wifi.auth: the applied authmode (wifi_auth_mode_t) as a small enum; known false = not connected
typedef enum {
    WIFI_AUTH_CLASS_NONE, WIFI_AUTH_CLASS_OPEN, WIFI_AUTH_CLASS_WEP, WIFI_AUTH_CLASS_WPA, WIFI_AUTH_CLASS_WPA2,
    WIFI_AUTH_CLASS_WPA_WPA2, WIFI_AUTH_CLASS_WPA3, WIFI_AUTH_CLASS_WPA2_WPA3, WIFI_AUTH_CLASS_WPA_ENT,
    WIFI_AUTH_CLASS_WPA2_ENT, WIFI_AUTH_CLASS_WPA3_ENT, WIFI_AUTH_CLASS_OTHER,
    WIFI_AUTH_CLASS_COUNT
} wifi_auth_class_t;
wifi_auth_class_t wifi_auth_class(int authmode, bool known);
// The scanned authmode is an Enterprise one (WPA, WPA2 or WPA3-Enterprise): the Wi-Fi screen then asks for the login
// instead of a password
bool wifi_auth_mode_is_enterprise(int authmode);

// What applying a pair to the driver does (pure, so the decision is host-tested). is_eap_entry: the saved list says the
// network is Enterprise; record_ok: its record was read and decoded; pass_is_mark: the pair's password is the
// Enterprise marker (the list entry is gone or lost its kind). Fail closed: an Enterprise entry without a usable record,
// or a marker with no Enterprise entry, never becomes a PSK or open network.
typedef enum { WIFI_PLAN_PSK, WIFI_PLAN_PSK_CLEAR, WIFI_PLAN_EAP, WIFI_PLAN_FAIL } wifi_plan_t;
wifi_plan_t wifi_apply_plan(bool is_eap_entry, bool record_ok, bool pass_is_mark, bool enterprise_on);
// Whether the C6 holds Enterprise state after an Enterprise apply: it did already, or at least one EAP call got through.
// An old C6 fails the first call, so it stays false and a PSK network never sends it the clear RPCs.
bool wifi_enterprise_after(bool was_on, bool touched);
// What a refused Enterprise apply undoes, in this order: CLEAR (Enterprise off and its credentials, only when an EAP call
// got through) before RESTORE (the previous station config, only when it was read and the new one had been set). The
// caller marks the pair blocked after both, never before: a restore never runs while the pair is blocked.
#define WIFI_UNWIND_CLEAR   1u
#define WIFI_UNWIND_RESTORE 2u
unsigned wifi_apply_unwind(bool touched, bool have_prev, bool cfg_set);

// What to do about the server name on the C6 before an Enterprise join. The driver has no call that clears it alone (the
// host side of RPC 382 refuses an empty name), only esp_wifi_sta_enterprise_disable drops it with the rest. So a credential
// with a name just sets it (it replaces the old one), one without a name sends nothing unless an earlier join left a name
// on the C6: then Enterprise is switched off first, so the old name cannot make this network fail its check.
typedef enum { WIFI_NAME_NONE, WIFI_NAME_SET, WIFI_NAME_DISABLE_FIRST } wifi_name_plan_t;
wifi_name_plan_t wifi_eap_name_plan(bool has_name, bool name_on_c6);

// `fw wifi eap <ssid> <peap|ttls> <mschapv2|pap> <identity|-> <username> <ca-path|-> <server-name|-> <password...>` (the text
// after "eap "): "-" is an empty identity, no CA or no server name; the password is the rest of the line and may hold
// spaces. ssid gets 33 bytes.
typedef enum {
    WIFI_EAP_PARSE_OK, WIFI_EAP_PARSE_USAGE, WIFI_EAP_PARSE_INVALID, WIFI_EAP_PARSE_PAP_NO_CA, WIFI_EAP_PARSE_PAP_NO_NAME
} wifi_eap_parse_t;
wifi_eap_parse_t wifi_eap_parse_line(char const *args, char ssid[33], wifi_eap_t *e);
