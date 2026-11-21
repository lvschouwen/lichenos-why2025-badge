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
// 7 header bytes ("E1", method, phase2 and their separators), 3 separators between the four fields, the fields, the NUL
#define WIFI_EAP_BLOB_MAX \
    (7 + 3 + WIFI_EAP_IDENTITY_MAX + WIFI_EAP_USER_MAX + WIFI_EAP_PASS_MAX + WIFI_EAP_CA_MAX + 1)

typedef enum { WIFI_EAP_PEAP = 0, WIFI_EAP_TTLS = 1 } wifi_eap_method_t;
typedef enum { WIFI_EAP_P2_MSCHAPV2 = 0, WIFI_EAP_P2_PAP = 1 } wifi_eap_phase2_t;

typedef struct {
    uint8_t method; // wifi_eap_method_t
    uint8_t phase2; // wifi_eap_phase2_t; PEAP is always MSCHAPV2
    char    identity[WIFI_EAP_IDENTITY_MAX + 1];
    char    username[WIFI_EAP_USER_MAX + 1];
    char    password[WIFI_EAP_PASS_MAX + 1];
    char    ca_path[WIFI_EAP_CA_MAX + 1]; // empty = no CA: connect unverified, and the UI says so
} wifi_eap_t;

// A text field: no control bytes (< 0x20, 0x7F; the record's separator is one), UTF-8 bytes allowed, length in range
bool wifi_eap_text_valid(char const *s, size_t min, size_t max);
// Empty (no CA) or an absolute path under /storage/ ending in .pem or .crt, no empty, "." or ".." segment, no control
// bytes, at most WIFI_EAP_CA_MAX bytes. Whether the file exists is the join's business.
bool wifi_eap_ca_path_valid(char const *path);
// Method and inner method known and allowed together, username 1-63, identity 0-63, password 1-127, CA path valid
bool wifi_eap_valid(wifi_eap_t const *e);
// True when the join would not check the server: no CA set (the UI shows "unverified server")
bool wifi_eap_unverified(wifi_eap_t const *e);

// The record kept as one secret string (the password is in it): "E1" US method US phase2 US identity US username US
// password US ca_path with US = 0x1F and the digits '0'/'1'. Returns the length without the NUL, 0 for an invalid
// credential or a cap below the record's size. The leading "E1" is the version: a decode of another one fails.
size_t wifi_eap_encode(wifi_eap_t const *e, char *out, size_t cap);
// Parses and validates a record; on any failure *e is zeroed and false is returned
bool   wifi_eap_decode(char const *rec, wifi_eap_t *e);
void   wifi_eap_wipe(wifi_eap_t *e);
