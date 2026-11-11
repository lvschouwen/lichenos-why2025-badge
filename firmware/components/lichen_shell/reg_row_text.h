// A settings row's text for one registry key (#332, #333): the applied value, how old it is, and whether it differs from
// what is saved. Pure (host_tests/reg_row_text_test.c); the cached text is lichen_reg_cached_text's.
#pragma once

#include <stddef.h>

// "[<label>: ]<value>[<unit>] (3.2 s)" for a fresh read, "... (stale 4 s)" for an old one, "... (saved <saved>)" when
// the applied value is not the saved one, "[<label>: ]? (<why>)" when the key was never read, "[<label>: ]?" when the
// name is unknown (cached empty or NULL). A stale read never claims "differs": the value may have changed since.
// label, saved and unit may be NULL; saved is the value text as the cached text prints it (no unit).
size_t reg_row_text(char const *label, char const *cached, char const *saved, char const *unit, char *out, size_t cap);

// The multi-key lines (#333): several keys read from the cache joined into one row, "<label>: <line> (<age>)" with the
// first key's age, " (stale)" when any key is stale, "<label>: ? (<why>)" when any key is unread or unknown. cached[i]
// is lichen_reg_get's text for key i; every one of RR_LORA_KEYS / RR_WIFI_KEYS must be given (NULL reads as unknown).
#define RR_LORA_KEYS 5 // lora.freq (Hz), lora.bw (Hz), lora.sf, lora.cr, lora.power (dBm)
#define RR_WIFI_KEYS 5 // wifi.country, wifi.txpwr (qdBm), wifi.bw (MHz), wifi.protocol (bitmask), wifi.ps (none/min)
size_t reg_lora_line(char const *label, char const *const cached[RR_LORA_KEYS], char *out, size_t cap);
size_t reg_wifi_line(char const *label, char const *const cached[RR_WIFI_KEYS], char *out, size_t cap);
// One Hz key as MHz ("80 MHz", "869.618 MHz"), with the same age / stale / "(wants <want_mhz>)" tail as reg_row_text
// ("wants" instead of "saved": the PSRAM clock against the speed it was asked for, psram.want in MHz)
// A cached text's value alone: "80" for "80 (3.0 s ago)", "" for an unread or unknown key
size_t reg_row_value(char const *cached, char *out, size_t cap);
size_t reg_mhz_text(char const *label, char const *cached_hz, char const *want_mhz, char *out, size_t cap);
