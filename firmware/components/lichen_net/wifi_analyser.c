#include "wifi_analyser.h"

#include <string.h>

void wifi_chan_stats(wifi_ap_t const *aps, size_t n, wifi_chan_stats_t *out) {
    memset(out->count, 0, sizeof(out->count));
    memset(out->best, WIFI_RSSI_NONE, sizeof(out->best));
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = aps[i].channel;
        if (ch < 1 || ch > WIFI_AN_CHANNELS) {
            continue;
        }
        if (out->count[ch] < UINT8_MAX) {
            out->count[ch]++;
        }
        if (aps[i].rssi > out->best[ch]) {
            out->best[ch] = aps[i].rssi;
        }
    }
}

void wifi_meter_clear(wifi_meter_t *m) {
    memset(m, 0, sizeof(*m));
}

void wifi_meter_push(wifi_meter_t *m, int8_t rssi) {
    m->v[m->head] = rssi;
    m->head       = (uint8_t)((m->head + 1) % WIFI_METER_LEN);
    if (m->n < WIFI_METER_LEN) {
        m->n++;
    }
}

int8_t wifi_meter_at(wifi_meter_t const *m, size_t i) {
    if (i >= m->n) {
        return WIFI_RSSI_NONE;
    }
    return m->v[(m->head + WIFI_METER_LEN - m->n + i) % WIFI_METER_LEN];
}

bool wifi_an_lookup(wifi_ap_t const *aps, size_t n, char const *ssid, wifi_ap_t *out) {
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(aps[i].ssid, ssid) == 0 && (!found || aps[i].rssi > out->rssi)) {
            *out  = aps[i];
            found = true;
        }
    }
    return found;
}

int wifi_level_pct(int8_t rssi) {
    if (rssi == WIFI_RSSI_NONE || rssi <= -100) {
        return 0;
    }
    return rssi >= -30 ? 100 : (rssi + 100) * 100 / 70;
}

bool wifi_an_scan_due(bool ever, uint32_t last_ms, uint32_t now_ms) {
    return !ever || (uint32_t)(now_ms - last_ms) >= WIFI_AN_SCAN_GAP_MS;
}
