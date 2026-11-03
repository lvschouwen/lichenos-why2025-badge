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

bool wifi_saved_forget(wifi_saved_t *l, char const *ssid) {
    int at = wifi_saved_find(l, ssid);
    if (at < 0) {
        return false;
    }
    remove_at(l, (size_t)at);
    return true;
}

bool wifi_saved_migrate(wifi_saved_t *l, char const *old_ssid, char const *old_pass) {
    return l->n == 0 && wifi_saved_add(l, old_ssid, old_pass);
}

int wifi_saved_pick(wifi_saved_t const *l, wifi_ap_t const *aps, size_t n, uint32_t skip_mask) {
    int    best      = -1;
    int    best_rssi = -1000;
    for (size_t i = 0; i < n; i++) {
        int at = wifi_saved_find(l, aps[i].ssid);
        if (at < 0 || (skip_mask >> at & 1) || (aps[i].open && l->e[at].pass[0])) {
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
