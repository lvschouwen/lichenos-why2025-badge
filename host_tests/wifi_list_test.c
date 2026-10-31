// Host test: Wi-Fi scan list cleaning and signal bars (#66)
#include "wifi_list.h"

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

static wifi_ap_t ap(char const *ssid, int rssi, bool open) {
    wifi_ap_t a = {0};
    snprintf(a.ssid, sizeof(a.ssid), "%s", ssid);
    a.rssi = (int8_t)rssi;
    a.open = open;
    return a;
}

int main(void) {
    wifi_ap_t list[] = {ap("home", -70, false), ap("", -30, true), ap("cafe", -60, true), ap("home", -50, false),
                        ap("far", -90, false)};
    size_t    n      = wifi_list_clean(list, 5);
    CHECK(n == 3);
    CHECK(strcmp(list[0].ssid, "home") == 0 && list[0].rssi == -50);
    CHECK(strcmp(list[1].ssid, "cafe") == 0 && list[1].open);
    CHECK(strcmp(list[2].ssid, "far") == 0);

    wifi_ap_t only_empty[] = {ap("", -40, false)};
    CHECK(wifi_list_clean(only_empty, 1) == 0);
    CHECK(wifi_list_clean(only_empty, 0) == 0);

    CHECK(wifi_bars(-55) == 4 && wifi_bars(-56) == 3 && wifi_bars(-65) == 3 && wifi_bars(-66) == 2);
    CHECK(wifi_bars(-75) == 2 && wifi_bars(-76) == 1 && wifi_bars(-85) == 1 && wifi_bars(-86) == 0);
    CHECK(wifi_bars(-30) == 4 && wifi_bars(-127) == 0);

    // An open twin with a stronger signal must not make a secured network look open
    wifi_ap_t twin[] = {ap("twin", -40, true), ap("twin", -70, false), ap("solo", -80, true)};
    CHECK(wifi_list_clean(twin, 3) == 2);
    CHECK(strcmp(twin[0].ssid, "twin") == 0 && twin[0].rssi == -40 && !twin[0].open);
    CHECK(twin[1].open);
    wifi_ap_t twin2[] = {ap("twin", -70, false), ap("twin", -40, true)};
    CHECK(wifi_list_clean(twin2, 2) == 1 && !twin2[0].open && twin2[0].rssi == -40);

    // SSIDs come from the air: control bytes and broken UTF-8 are escaped, valid UTF-8 stays
    char out[64];
    CHECK(wifi_ssid_sanitize("home", out, sizeof(out)) == 4 && strcmp(out, "home") == 0);
    wifi_ssid_sanitize("a\033[2Jb\x7f", out, sizeof(out));
    CHECK(strcmp(out, "a\\x1b[2Jb\\x7f") == 0);
    wifi_ssid_sanitize("caf\xc3\xa9", out, sizeof(out));
    CHECK(strcmp(out, "caf\xc3\xa9") == 0);
    wifi_ssid_sanitize("x\xc3(y\xff", out, sizeof(out));
    CHECK(strcmp(out, "x\\xc3(y\\xff") == 0);
    wifi_ssid_sanitize("\033\033\033\033\033", out, 10); // truncated, always terminated, no half escape
    CHECK(strcmp(out, "\\x1b\\x1b") == 0);

    // C1 controls (CSI U+009B), overlongs, surrogates and bidi controls are escaped even though they are "UTF-8"
    wifi_ssid_sanitize("a\xc2\x9b" "b", out, sizeof(out));
    CHECK(strcmp(out, "a\\xc2\\x9b" "b") == 0);
    wifi_ssid_sanitize("\xc0\xaf", out, sizeof(out));
    CHECK(strcmp(out, "\\xc0\\xaf") == 0);
    wifi_ssid_sanitize("\xed\xa0\x80", out, sizeof(out));
    CHECK(strcmp(out, "\\xed\\xa0\\x80") == 0);
    wifi_ssid_sanitize("\xe0\x80\xaf", out, sizeof(out)); // overlong slash
    CHECK(strcmp(out, "\\xe0\\x80\\xaf") == 0);
    wifi_ssid_sanitize("\xf4\x90\x80\x80", out, sizeof(out)); // above U+10FFFF
    CHECK(strcmp(out, "\\xf4\\x90\\x80\\x80") == 0);
    wifi_ssid_sanitize("\xe2\x80\xae", out, sizeof(out)); // U+202E right-to-left override
    CHECK(strcmp(out, "\\xe2\\x80\\xae") == 0);
    wifi_ssid_sanitize("\xe2\x81\xa6", out, sizeof(out)); // U+2066
    CHECK(strcmp(out, "\\xe2\\x81\\xa6") == 0);
    wifi_ssid_sanitize("\xe2\x82\xac \xf0\x9f\x93\xb6", out, sizeof(out)); // euro sign, antenna bars emoji
    CHECK(strcmp(out, "\xe2\x82\xac \xf0\x9f\x93\xb6") == 0);

    if (failures) {
        return 1;
    }
    printf("wifi_list_test: ok\n");
    return 0;
}
