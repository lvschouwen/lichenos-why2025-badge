// The applied LoRa radio's texts (#335): see mesh_applied.h
#include "mesh_applied.h"

#include "mesh_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char const *const mesh_applied_names[MESH_APPLIED_KEYS] = {"lora.freq", "lora.bw", "lora.sf", "lora.cr", "lora.power"};

// One cached text: its number into *value and its bracket ("1.2 s ago", "not read yet") into note. false when it is not
// "<number>" or "<number> (<note>)" (an unread "? (<why>)", an unknown key, anything malformed); note still gets the
// bracket, so an unread key's reason is kept
static bool parse_one(char const *cached, long long *value, char *note, size_t cap) {
    note[0] = '\0';
    if (!cached || !*cached) {
        return false;
    }
    char const *open = strstr(cached, " (");
    size_t      len  = strlen(cached);
    bool        bracket = open && len >= 3 && cached[len - 1] == ')' && open + 2 <= cached + len - 1;
    if (bracket) {
        size_t n = len - 1 - (size_t)(open + 2 - cached);
        snprintf(note, cap, "%.*s", (int)n, open + 2);
    }
    char *end = NULL;
    *value    = strtoll(cached, &end, 10); // the Hz keys are U32 and long is 32-bit on the P4; dBm can be < 0
    return end != cached && (*end == '\0' || (bracket && end == open));
}

void mesh_applied_parse(char const *const cached[MESH_APPLIED_KEYS], mesh_applied_t *out) {
    long long v[MESH_APPLIED_KEYS] = {0};
    char note[32];
    memset(out, 0, sizeof(*out));
    out->read = true;
    for (int i = 0; i < MESH_APPLIED_KEYS; i++) {
        if (!parse_one(cached[i], &v[i], note, sizeof(note))) {
            if (out->read) { // the first unread key says why
                snprintf(out->why, sizeof(out->why), "%s", note);
            }
            out->read = false;
            continue;
        }
        out->stale = out->stale || strncmp(note, "stale", 5) == 0;
        if (i == 0) {
            snprintf(out->age, sizeof(out->age), "%s", note);
        }
    }
    out->freq_hz = (uint32_t)v[0];
    out->bw_hz   = (uint32_t)v[1];
    out->sf      = (uint8_t)v[2];
    out->cr      = (uint8_t)v[3];
    out->dbm     = (int16_t)v[4];
}

static size_t unread_text(mesh_applied_t const *a, char *out, size_t cap) {
    if (*a->why) {
        snprintf(out, cap, "? (%s)", a->why);
    } else {
        snprintf(out, cap, "?");
    }
    return strlen(out);
}

size_t mesh_applied_radio(mesh_applied_t const *a, char *out, size_t cap) {
    if (!cap) {
        return 0;
    }
    if (!a->read) {
        return unread_text(a, out, cap);
    }
    char radio[64];
    mesh_format_radio(radio, sizeof(radio), a->freq_hz / 1e6, a->bw_hz / 1e3, a->sf, a->cr);
    snprintf(out, cap, "%s%s", radio, a->stale ? " (stale)" : "");
    return strlen(out);
}

size_t mesh_applied_power(mesh_applied_t const *a, char *out, size_t cap) {
    if (!cap) {
        return 0;
    }
    if (!a->read) {
        return unread_text(a, out, cap);
    }
    snprintf(out, cap, "%d dBm%s", a->dbm, a->stale ? " (stale)" : "");
    return strlen(out);
}

size_t mesh_applied_line(mesh_applied_t const *a, bool with_age, char *out, size_t cap) {
    if (!cap) {
        return 0;
    }
    if (!a->read) {
        return unread_text(a, out, cap);
    }
    char radio[64];
    mesh_format_radio(radio, sizeof(radio), a->freq_hz / 1e6, a->bw_hz / 1e3, a->sf, a->cr);
    if (with_age && *a->age) {
        snprintf(out, cap, "%s, %d dBm (%s)", radio, a->dbm, a->age);
    } else {
        snprintf(out, cap, "%s, %d dBm%s", radio, a->dbm, a->stale ? " (stale)" : "");
    }
    return strlen(out);
}

bool mesh_applied_differs(mesh_applied_t const *a, float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr, int dbm) {
    if (!a->read || a->stale) {
        return false;
    }
    double freq_off = fabs((double)a->freq_hz - (double)freq_mhz * 1e6);
    double bw_off   = fabs((double)a->bw_hz - (double)bw_khz * 1e3);
    return freq_off > 500.0 || bw_off > 50.0 || a->sf != sf || a->cr != cr || a->dbm != dbm;
}
