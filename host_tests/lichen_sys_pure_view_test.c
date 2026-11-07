// Host test: the ESP-free half of lichen_sys_clock / lichen_sys_net / lichen_sys_boot (#244). The gate's calls must
// answer exactly what the components' own functions do, since the shell's views now only see the gate. The type
// mirrors are checked at compile time by lichen_sys_pure_view.c itself.
#include "lichen_sys_boot.h"
#include "lichen_sys_clock.h"
#include "lichen_sys_keys.h"
#include "lichen_sys_net.h"

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

static void test_clock(void) {
    CHECK(lichen_sys_clock_days_in_month(2024, 2) == 29 && lichen_sys_clock_days_in_month(2025, 2) == 28);
    CHECK(lichen_sys_clock_days_in_month(2025, 4) == 30 && lichen_sys_clock_days_in_month(2025, 12) == 31);
    int64_t utc = 0;
    CHECK(lichen_sys_clock_local_to_utc(2025, 2, 30, 12, 0, &utc) == LICHEN_CLOCK_LOCAL_BAD_DATE);
    CHECK(lichen_sys_clock_local_hint(LICHEN_CLOCK_LOCAL_OK) == NULL);
    CHECK(lichen_sys_clock_local_hint(LICHEN_CLOCK_LOCAL_GAP) != NULL);
    CHECK(lichen_sys_clock_back_hint(1000, 999) == NULL && lichen_sys_clock_back_hint(10000, 1000) != NULL);
    CHECK(strcmp(lichen_sys_clock_verdict_text(LICHEN_CLOCK_OK), "ok") == 0);
    CHECK(lichen_sys_clock_in_window(1800000000, 1800000000) && !lichen_sys_clock_in_window(1000, 1800000000));
    CHECK(lichen_sys_clock_accepted(LICHEN_CLOCK_OK) && lichen_sys_clock_accepted(LICHEN_CLOCK_RECOVERED) &&
          !lichen_sys_clock_accepted(LICHEN_CLOCK_OUTRANKED));
}

static void test_net(void) {
    CHECK(lichen_sys_wifi_bars(-50) == 4 && lichen_sys_wifi_bars(-60) == 3 && lichen_sys_wifi_bars(-70) == 2);
    CHECK(lichen_sys_wifi_bars(-80) == 1 && lichen_sys_wifi_bars(-90) == 0);
    CHECK(lichen_sys_wifi_level_pct(-100) == 0 && lichen_sys_wifi_level_pct(-30) == 100);
    CHECK(lichen_sys_wifi_cred_valid("home", "") && lichen_sys_wifi_cred_valid("home", "12345678"));
    CHECK(!lichen_sys_wifi_cred_valid("", "12345678") && !lichen_sys_wifi_cred_valid("home", "short"));
    char out[16];
    CHECK(lichen_sys_wifi_ssid_sanitize("a\x01" "b", out, sizeof(out)) == 6 && strcmp(out, "a\\x01b") == 0);

    lichen_sys_wifi_ap_t aps[3] = {{.ssid = "b", .rssi = -70, .channel = 6},
                                   {.ssid = "", .rssi = -40, .channel = 1},
                                   {.ssid = "a", .rssi = -50, .channel = 6}};
    CHECK(lichen_sys_wifi_list_clean(aps, 3) == 2 && strcmp(aps[0].ssid, "a") == 0 && strcmp(aps[1].ssid, "b") == 0);
    lichen_sys_wifi_chan_stats_t cs;
    lichen_sys_wifi_chan_stats(aps, 2, &cs);
    CHECK(cs.count[6] == 2 && cs.best[6] == -50 && cs.count[1] == 0 && cs.best[1] == LICHEN_SYS_WIFI_RSSI_NONE);
    lichen_sys_wifi_ap_t found;
    CHECK(lichen_sys_wifi_an_lookup(aps, 2, "b", &found) && found.rssi == -70);
    CHECK(!lichen_sys_wifi_an_lookup(aps, 2, "zz", &found));

    lichen_sys_wifi_meter_t m;
    lichen_sys_wifi_meter_clear(&m);
    lichen_sys_wifi_meter_push(&m, -60);
    lichen_sys_wifi_meter_push(&m, -61);
    CHECK(lichen_sys_wifi_meter_at(&m, 0) == -60 && lichen_sys_wifi_meter_at(&m, 1) == -61);
    CHECK(lichen_sys_wifi_meter_at(&m, 2) == LICHEN_SYS_WIFI_RSSI_NONE);
    CHECK(lichen_sys_wifi_an_scan_due(false, 0, 0) && !lichen_sys_wifi_an_scan_due(true, 1000, 2000));
    CHECK(lichen_sys_wifi_an_scan_due(true, 1000, 1000 + LICHEN_SYS_WIFI_AN_SCAN_GAP_MS));
}

static void test_boot(void) {
    lichen_boot_view_t v;
    memset(&v, 0, sizeof(v));
    CHECK(lichen_sys_boot_overall_pct(&v) == 0);
    for (int i = 0; i < LICHEN_BOOT_STEP_COUNT; i++) {
        v.state[i] = LICHEN_STEP_DONE;
    }
    CHECK(lichen_sys_boot_overall_pct(&v) == 100);
    char buf[64];
    CHECK(lichen_sys_boot_sentence(LICHEN_BOOT_C6, -1, buf, sizeof(buf)) == buf && buf[0] != '\0');
    char text[] = "\x1b[31mred\x1b[0m\r\n";
    size_t n = lichen_sys_log_clean(text, strlen(text), text);
    CHECK(n == 3 && strcmp(text, "red") == 0);
}

int main(void) {
    test_clock();
    test_net();
    test_boot();
    if (failures) {
        printf("lichen_sys_pure_view_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("lichen_sys_pure_view_test: all passed\n");
    return 0;
}
