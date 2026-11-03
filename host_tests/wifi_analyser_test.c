// Host test: the Wi-Fi analyser's maths (#159): channel counts, the signal history ring, the scan rate limit
#include "wifi_analyser.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static wifi_ap_t ap(char const *ssid, int rssi, int ch) {
    wifi_ap_t a = {0};
    strcpy(a.ssid, ssid);
    a.rssi    = (int8_t)rssi;
    a.channel = (uint8_t)ch;
    return a;
}

static void channels(void) {
    wifi_ap_t aps[] = {ap("a", -60, 1), ap("a", -40, 6), ap("", -80, 6), ap("b", -70, 6), ap("c", -50, 13),
                       ap("d", -30, 0), ap("e", -30, 14), ap("f", -30, 36)};
    wifi_chan_stats_t st;
    wifi_chan_stats(aps, sizeof(aps) / sizeof(aps[0]), &st);
    CHECK(st.count[1] == 1 && st.best[1] == -60);
    CHECK(st.count[6] == 3 && st.best[6] == -40); // the same SSID on two channels counts on both; hidden counts
    CHECK(st.count[13] == 1 && st.best[13] == -50);
    CHECK(st.count[2] == 0 && st.best[2] == WIFI_RSSI_NONE);
    int total = 0;
    for (int c = 0; c <= WIFI_AN_CHANNELS; c++) {
        total += st.count[c];
    }
    CHECK(total == 5); // channel 0, 14 and 5 GHz channels are not drawn
    wifi_chan_stats(aps, 0, &st);
    CHECK(st.count[6] == 0);
}

static void ring(void) {
    wifi_meter_t m;
    wifi_meter_clear(&m);
    CHECK(wifi_meter_at(&m, 0) == WIFI_RSSI_NONE);
    wifi_meter_push(&m, -50);
    wifi_meter_push(&m, -60);
    wifi_meter_push(&m, WIFI_RSSI_NONE); // missed in one scan
    CHECK(m.n == 3 && wifi_meter_at(&m, 0) == -50 && wifi_meter_at(&m, 2) == WIFI_RSSI_NONE && wifi_meter_at(&m, 3) == WIFI_RSSI_NONE);
    for (int i = 0; i < WIFI_METER_LEN + 5; i++) {
        wifi_meter_push(&m, (int8_t)(-30 - i));
    }
    CHECK(m.n == WIFI_METER_LEN);
    CHECK(wifi_meter_at(&m, WIFI_METER_LEN - 1) == -30 - (WIFI_METER_LEN + 4)); // newest last
    CHECK(wifi_meter_at(&m, 0) == -30 - 5); // the oldest 60 are kept
}

static void lookup_and_level(void) {
    wifi_ap_t aps[] = {ap("home", -70, 1), ap("home", -55, 11), ap("other", -20, 6)}, out;
    CHECK(wifi_an_lookup(aps, 3, "home", &out) && out.rssi == -55 && out.channel == 11);
    CHECK(!wifi_an_lookup(aps, 3, "gone", &out));
    CHECK(wifi_level_pct(-30) == 100 && wifi_level_pct(-20) == 100 && wifi_level_pct(-100) == 0 && wifi_level_pct(-110) == 0);
    CHECK(wifi_level_pct(WIFI_RSSI_NONE) == 0 && wifi_level_pct(-65) == 50);
}

static void rate_limit(void) {
    CHECK(wifi_an_scan_due(false, 0, 0)); // the first scan runs at once
    CHECK(!wifi_an_scan_due(true, 1000, 1000 + WIFI_AN_SCAN_GAP_MS - 1));
    CHECK(wifi_an_scan_due(true, 1000, 1000 + WIFI_AN_SCAN_GAP_MS));
    CHECK(!wifi_an_scan_due(true, 0xFFFFFFF0u, 5)); // the clock wrapped 21 ms ago
    CHECK(wifi_an_scan_due(true, 0xFFFFFFF0u, WIFI_AN_SCAN_GAP_MS));
}

int main(void) {
    channels();
    ring();
    lookup_and_level();
    rate_limit();
    if (failures) {
        return 1;
    }
    printf("wifi_analyser_test: ok\n");
    return 0;
}
