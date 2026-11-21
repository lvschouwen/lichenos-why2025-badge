#pragma once

#include "lichen_sys_net.h"

#include <stdbool.h>
#include <stddef.h>

// The Enterprise login form of the Wi-Fi screen (#496), ESP-free and LVGL-free: what each row shows, what a typed value
// must look like, and what the screen says before it asks the Wi-Fi service to join. The password is write-only (R3):
// no function here puts it into text, a row only says whether one is set.

typedef enum {
    EAP_FIELD_METHOD,   // PEAP or TTLS (a cycle row)
    EAP_FIELD_INNER,    // MSCHAPv2 or PAP (a cycle row; PEAP has only MSCHAPv2)
    EAP_FIELD_IDENTITY, // the outer name, sent in clear; empty = the username goes out instead
    EAP_FIELD_USER,
    EAP_FIELD_PASSWORD,
    EAP_FIELD_CA,       // a /storage/ path to the server's CA certificate; empty = none
    EAP_FIELD_SERVER,   // the host name the server certificate must carry; empty = no name check
    EAP_FIELD_COUNT
} eap_field_t;

// A row's value is at most this many bytes (with the ellipsis): the row keeps its value visible (R3), the full text is in
// the entry that opens on Enter
#define EAP_VALUE_MAX 24

// A fresh form: PEAP, MSCHAPv2, every text field empty
void eap_form_init(lichen_sys_wifi_eap_t *e);
// Method to the other one; PEAP always has the MSCHAPv2 inner method, so going to PEAP resets it
void eap_form_cycle_method(lichen_sys_wifi_eap_t *e);
// Inner method to the other one; false (nothing changed) for PEAP, which has no choice
bool eap_form_cycle_inner(lichen_sys_wifi_eap_t *e);
// The row's value text for out (cap at least EAP_VALUE_MAX + 1): "PEAP", "MSCHAPv2", a text cut with "..." at a character
// boundary, the CA file's name without its directory, "Not set" for an empty one; the password is only "Set" / "Not set"
void eap_form_value(lichen_sys_wifi_eap_t const *e, eap_field_t f, char *out, size_t cap);
// The field's own limits for a typed text: NULL when it is fine, else the refusal in plain words. Never echoes the text.
char const *eap_form_field_error(eap_field_t f, char const *text);
// Stores a typed text into the field (call eap_form_field_error first); false for a field that takes no text
bool eap_form_set(lichen_sys_wifi_eap_t *e, eap_field_t f, char const *text);
// The most the entry for the field takes
size_t eap_form_field_max(eap_field_t f);
// The title of the entry for the field: "Identity", "Username", ...
char const *eap_form_field_name(eap_field_t f);
// A short parenthesis for the entry's title, "" for none: what the field is for and what empty means (R5)
char const *eap_form_field_hint(eap_field_t f);
// Whether the login may be sent: true, or false with the refusal (plain words, what to fix) in why
bool eap_form_check(lichen_sys_wifi_eap_t const *e, char *why, size_t cap);
// What the join will not check about the server, in words, "" when it checks both the CA and the name (R8: never colour alone)
char const *eap_form_warning(lichen_sys_wifi_eap_t const *e);
