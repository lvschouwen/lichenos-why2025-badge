#include "wifi_saved.h"

#include <string.h>

int wifi_saved_find(wifi_saved_t const *l, char const *ssid) {
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->e[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void remove_at(wifi_saved_t *l, size_t i) {
    for (; i + 1 < l->n; i++) {
        l->e[i] = l->e[i + 1];
    }
    memset(&l->e[--l->n], 0, sizeof(l->e[0]));
}

bool wifi_saved_add(wifi_saved_t *l, char const *ssid, char const *pass) {
    if (!wifi_cred_valid(ssid, pass)) {
        return false;
    }
    int at = wifi_saved_find(l, ssid);
    if (at >= 0) {
        remove_at(l, (size_t)at);
    } else if (l->n == WIFI_SAVED_MAX) {
        remove_at(l, l->n - 1);
    }
    for (size_t i = l->n; i > 0; i--) {
        l->e[i] = l->e[i - 1];
    }
    memset(&l->e[0], 0, sizeof(l->e[0]));
    strcpy(l->e[0].ssid, ssid);
    strcpy(l->e[0].pass, pass);
    l->n++;
    return true;
}

bool wifi_saved_add_eap(wifi_saved_t *l, char const *ssid) {
    if (!wifi_cred_valid(ssid, "")) {
        return false;
    }
    wifi_saved_add(l, ssid, ""); // the same list rules: front, replace by SSID, oldest goes
    // At most WIFI_SAVED_MAX - 1 other entries hold a record slot, so one is free
    uint8_t taken = 0;
    for (size_t i = 1; i < l->n; i++) {
        if (l->e[i].kind == WIFI_SAVED_EAP) {
            taken |= (uint8_t)(1u << l->e[i].rec);
        }
    }
    uint8_t rec = 0;
    while (rec < WIFI_SAVED_MAX - 1 && (taken >> rec & 1)) {
        rec++;
    }
    l->e[0].kind = WIFI_SAVED_EAP;
    l->e[0].rec  = rec;
    return true;
}

uint8_t wifi_saved_kind_code(wifi_saved_entry_t const *e) {
    return e->kind == WIFI_SAVED_EAP ? (uint8_t)(1 + e->rec) : 0;
}

bool wifi_saved_slot_parse(wifi_saved_entry_t *e, bool have_pass, uint8_t code, uint8_t *used) {
    e->kind = WIFI_SAVED_PSK;
    e->rec  = 0;
    if (code == 0) {
        return have_pass && wifi_cred_valid(e->ssid, e->pass);
    }
    uint8_t rec = (uint8_t)(code - 1);
    if (code > WIFI_SAVED_MAX || (*used >> rec & 1) || !wifi_cred_valid(e->ssid, "")) {
        return false;
    }
    e->kind    = WIFI_SAVED_EAP;
    e->rec     = rec;
    e->pass[0] = '\0'; // whatever the store held (the marker, or nothing): an Enterprise entry has no PSK
    *used |= (uint8_t)(1u << rec);
    return true;
}

uint8_t wifi_saved_rec_mask(wifi_saved_t const *l) {
    uint8_t mask = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (l->e[i].kind == WIFI_SAVED_EAP) {
            mask |= (uint8_t)(1u << l->e[i].rec);
        }
    }
    return mask;
}

int wifi_saved_eap_rec(wifi_saved_t const *l, char const *ssid) {
    int at = wifi_saved_find(l, ssid);
    return wifi_saved_is_eap(l, at) ? l->e[at].rec : -1;
}

bool wifi_saved_promote(wifi_saved_t *l, int index) {
    if (index < 0 || (size_t)index >= l->n) {
        return false;
    }
    wifi_saved_entry_t keep = l->e[index];
    for (int i = index; i > 0; i--) {
        l->e[i] = l->e[i - 1];
    }
    l->e[0] = keep;
    memset(&keep, 0, sizeof(keep));
    return true;
}

bool wifi_saved_is_eap(wifi_saved_t const *l, int index) {
    return index >= 0 && (size_t)index < l->n && l->e[index].kind == WIFI_SAVED_EAP;
}

bool wifi_saved_forget(wifi_saved_t *l, char const *ssid) {
    int at = wifi_saved_find(l, ssid);
    if (at < 0) {
        return false;
    }
    remove_at(l, (size_t)at);
    return true;
}

int wifi_saved_pick(wifi_saved_t const *l, wifi_ap_t const *aps, size_t n, uint32_t skip_mask) {
    int    best      = -1;
    int    best_rssi = -1000;
    for (size_t i = 0; i < n; i++) {
        int at = wifi_saved_find(l, aps[i].ssid);
        // A saved open network (empty password) is never joined by itself: anyone can broadcast its name. Secured ones
        // only match an AP that is secured too (an open twin of a secured name is a trap)
        bool secured = at >= 0 && (l->e[at].pass[0] != '\0' || l->e[at].kind == WIFI_SAVED_EAP);
        if (at < 0 || (skip_mask >> at & 1) || !secured || aps[i].open) {
            continue;
        }
        if (aps[i].rssi > best_rssi) {
            best_rssi = aps[i].rssi;
            best      = at;
        }
    }
    return best;
}

void wifi_saved_wipe(wifi_saved_t *l) {
    memset(l, 0, sizeof(*l));
}
