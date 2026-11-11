#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The LoRa radio as the C6 last applied it (#335), next to what MeshCore asks for (its NodePrefs). The values come from
// the registry cache (lichen_reg_get, #331: never waits for the C6). Pure logic, no ESP headers (host_tests/
// mesh_applied_test.c). Settings' own line builder (lichen_shell's reg_row_text.c) is not reachable from an app (#84),
// and the app keeps its one radio text (R4, mesh_format_radio).
#define MESH_APPLIED_KEYS 5
// lora.freq (Hz), lora.bw (Hz), lora.sf, lora.cr, lora.power (dBm): the `fw get` names, in mesh_applied_parse's order
extern char const *const mesh_applied_names[MESH_APPLIED_KEYS];

typedef struct {
    bool     read;  // every key has a value
    bool     stale; // a value is old: the C6 stopped answering, it may have changed since
    uint32_t freq_hz, bw_hz;
    uint8_t  sf, cr;
    int16_t  dbm;
    char     why[32]; // !read: the first unread key's reason ("not read yet"), "" for a key the registry lacks
    char     age[32]; // read: the frequency's age as the cache prints it ("1.2 s ago", "stale, 9 s ago")
} mesh_applied_t;

// cached[i] is lichen_reg_get's text for mesh_applied_names[i] ("869618000 (1.2 s ago)", "? (not read yet)"), NULL
// when the registry does not have the key
void mesh_applied_parse(char const *const cached[MESH_APPLIED_KEYS], mesh_applied_t *out);

// "869.618 MHz, 62.5 kHz, SF7, CR5", "... (stale)"; "? (not read yet)" (the cache's reason) or "?" when unread
size_t mesh_applied_radio(mesh_applied_t const *a, char *out, size_t cap);
// "22 dBm", "22 dBm (stale)", or the same "?" texts
size_t mesh_applied_power(mesh_applied_t const *a, char *out, size_t cap);
// Both in one: "869.618 MHz, 62.5 kHz, SF7, CR5, 22 dBm", with the age in brackets when with_age ("(1.2 s ago)")
size_t mesh_applied_line(mesh_applied_t const *a, bool with_age, char *out, size_t cap);

// Does the applied radio differ from MeshCore's (freq MHz, bw kHz, as mesh_settings_t holds them)? false while a key
// is unread or stale: an old value proves nothing. The frequency within 500 Hz and the bandwidth within 50 Hz count
// as the same (MeshCore's float MHz does not convert to whole Hz, and the screens show kHz).
bool mesh_applied_differs(mesh_applied_t const *a, float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr, int dbm);

#ifdef __cplusplus
}
#endif
