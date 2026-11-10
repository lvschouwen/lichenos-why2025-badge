#include "wifi_regdom.h"

#include <stdio.h>
#include <string.h>

static size_t done(int n, size_t cap) {
    if (n < 0 || !cap) {
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

static bool alnum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

size_t wifi_country_text(char const cc[3], char *out, size_t cap) {
    if (cap < 3) {
        if (cap) {
            out[0] = '\0';
        }
        return 0;
    }
    // The third octet (' ' all environments, 'O' outdoor, 'I' indoor, 'X' non-country) is not part of the code
    out[0] = alnum(cc[0]) ? cc[0] : '?';
    out[1] = alnum(cc[1]) ? cc[1] : '?';
    out[2] = '\0';
    return 2;
}

bool wifi_regdom_expected(wifi_readback_t const *r) {
    char cc[3];
    wifi_country_text(r->cc, cc, sizeof(cc));
    return r->country_ok && strcmp(cc, WIFI_COUNTRY) == 0 && r->schan == WIFI_COUNTRY_SCHAN &&
           r->nchan == WIFI_COUNTRY_NCHAN && r->policy == WIFI_RB_POLICY_MANUAL;
}

size_t wifi_regdom_text(wifi_readback_t const *r, char *buf, size_t cap) {
    if (!r->country_ok) {
        return done(snprintf(buf, cap, "not read"), cap);
    }
    char cc[3];
    wifi_country_text(r->cc, cc, sizeof(cc));
    unsigned last = (unsigned)r->schan + r->nchan - (r->nchan ? 1u : 0u);
    return done(snprintf(buf, cap, "%s, channels %u-%u, max %d dBm, policy %s", cc, (unsigned)r->schan, last,
                         (int)r->country_max_dbm, r->policy == WIFI_RB_POLICY_MANUAL ? "manual" : "auto (802.11d)"),
                cap);
}

uint8_t wifi_bw_to_mhz(int bw) {
    switch (bw) {
        case 1: return 20;  // WIFI_BW20
        case 2: return 40;  // WIFI_BW40
        case 3: return 80;  // WIFI_BW80
        case 4:             // WIFI_BW160
        case 5: return 160; // WIFI_BW80_BW80
        default: return 0;
    }
}

size_t wifi_tx_reply(wifi_readback_t const *r, char *buf, size_t cap) {
    char const *how = r->req_tx_raw ? "set" : "default";
    if (!r->tx_ok) {
        return done(snprintf(buf, cap, "wifi: max TX power %s, applied not readable (no C6 link?)", how), cap);
    }
    int raw = r->tx_raw;
    int mag = raw < 0 ? -raw : raw;
    return done(snprintf(buf, cap, "wifi: max TX power %s%d.%02d dBm applied (%s; not saved)", raw < 0 ? "-" : "",
                         mag / 4, (mag % 4) * 25, how),
                cap);
}

size_t wifi_bw_reply(wifi_readback_t const *r, unsigned asked_mhz, bool reconnecting, char *buf, size_t cap) {
    char now[24];
    if (!r->bw_ok) {
        snprintf(now, sizeof(now), "applied not readable");
    } else if (r->bw_mhz) {
        snprintf(now, sizeof(now), "%u MHz applied now", (unsigned)r->bw_mhz);
    } else {
        snprintf(now, sizeof(now), "other applied now");
    }
    if (!asked_mhz) {
        return done(snprintf(buf, cap, "wifi: bandwidth %s (fw wifi bw 20 | 40, not saved)", now), cap);
    }
    return done(snprintf(buf, cap, "wifi: bandwidth %u MHz asked, %s (%s)", asked_mhz, now,
                         reconnecting ? "reconnecting to apply, not saved" : "busy, try again"),
                cap);
}

size_t wifi_ps_reply(wifi_readback_t const *r, char *buf, size_t cap) {
    if (!r->ps_ok) {
        return done(snprintf(buf, cap, "wifi: power save not readable (no C6 link?)"), cap);
    }
    char const *name = r->ps == WIFI_RB_PS_NONE ? "none" : r->ps == WIFI_RB_PS_MIN ? "min" : "max";
    return done(snprintf(buf, cap, "wifi: power save %s applied (fw wifi ps none | min, not saved)", name), cap);
}
