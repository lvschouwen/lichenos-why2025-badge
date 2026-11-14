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

    // UTF-8 validator pinned at its boundaries (#212): each row is input, expected output
    static struct {
        char const *in;
        char const *want;
    } const utf[] = {
        {"abc", "abc"},                                  // ASCII
        {"\xc2\xa0", "\xc2\xa0"},                        // 2-byte, lowest kept
        {"\xdf\xbf", "\xdf\xbf"},                        // 2-byte, highest
        {"\xe0\xa0\x80", "\xe0\xa0\x80"},                // 3-byte, lowest
        {"\xed\x9f\xbf", "\xed\x9f\xbf"},                // 3-byte, just below the surrogates
        {"\xee\x80\x80", "\xee\x80\x80"},                // 3-byte, just above the surrogates
        {"\xef\xbf\xbf", "\xef\xbf\xbf"},                // 3-byte, highest
        {"\xf0\x90\x80\x80", "\xf0\x90\x80\x80"},        // 4-byte, lowest
        {"\xf4\x8f\xbf\xbf", "\xf4\x8f\xbf\xbf"},        // 4-byte, U+10FFFF
        {"\xc0\x80", "\\xc0\\x80"},                      // overlong 2-byte
        {"\xc1\xbf", "\\xc1\\xbf"},                      // overlong 2-byte, highest
        {"\xe0\x9f\xbf", "\\xe0\\x9f\\xbf"},             // overlong 3-byte
        {"\xf0\x8f\xbf\xbf", "\\xf0\\x8f\\xbf\\xbf"},    // overlong 4-byte
        {"\xed\xa0\x80", "\\xed\\xa0\\x80"},             // surrogate U+D800
        {"\xed\xbf\xbf", "\\xed\\xbf\\xbf"},             // surrogate U+DFFF
        {"\xf4\x90\x80\x80", "\\xf4\\x90\\x80\\x80"},    // above U+10FFFF
        {"\xf5\x80\x80\x80", "\\xf5\\x80\\x80\\x80"},    // F5 is never a lead byte
        {"\xf8\x88\x80\x80\x80", "\\xf8\\x88\\x80\\x80\\x80"}, // 5-byte form
        {"\x80", "\\x80"},                               // stray continuation
        {"a\xbf" "b", "a\\xbfb"},                        // stray continuation in text
        {"\xfe", "\\xfe"},
        {"\xff", "\\xff"},
        {"\xe2\x82", "\\xe2\\x82"},                      // truncated by the terminator
        {"\xf0\x9f\x93", "\\xf0\\x9f\\x93"},             // truncated 4-byte
        {"\xc3", "\\xc3"},                               // lone lead at the end
        {"\xe2\x82(", "\\xe2\\x82("},                    // lead then a non-continuation
        {"\xc2\x80", "\\xc2\\x80"},                      // C1 control U+0080
        {"\xc2\x9f", "\\xc2\\x9f"},                      // C1 control U+009F
        {"\xe2\x80\x8e", "\\xe2\\x80\\x8e"},             // U+200E
        {"\xe2\x80\x8f", "\\xe2\\x80\\x8f"},             // U+200F
        {"\xe2\x80\xaa", "\\xe2\\x80\\xaa"},             // U+202A
        {"\xe2\x81\xa9", "\\xe2\\x81\\xa9"},             // U+2069
        {"\xe2\x80\x8d", "\xe2\x80\x8d"},                // U+200D (joiner) stays
        {"\xe2\x81\xa5", "\xe2\x81\xa5"},                // U+2065 stays
        {"\xe2\x81\xaa", "\xe2\x81\xaa"},                // U+206A stays
        {"a\0\xc3\xa9", "a"},                            // an embedded NUL ends the string
    };
    for (size_t i = 0; i < sizeof(utf) / sizeof(utf[0]); i++) {
        wifi_ssid_sanitize(utf[i].in, out, sizeof(out));
        if (strcmp(out, utf[i].want) != 0) {
            printf("FAIL utf[%zu]: got '%s' want '%s'\n", i, out, utf[i].want);
            failures++;
        }
    }
    // A full 32-byte SSID of 3-byte characters fits exactly; a small cap stops before a sequence it cannot hold whole
    char big[33 + 1], full[40];
    memset(big, 0, sizeof(big));
    for (int i = 0; i < 10; i++) {
        memcpy(big + i * 3, "\xe2\x82\xac", 3);
    }
    big[30] = 'a';
    big[31] = 'b';
    CHECK(wifi_ssid_sanitize(big, full, sizeof(full)) == 32 && strcmp(full, big) == 0);
    CHECK(wifi_ssid_sanitize("\xe2\x82\xac", out, 3) == 0 && out[0] == '\0'); // needs 3 + NUL
    CHECK(wifi_ssid_sanitize("\xe2\x82\xac", out, 4) == 3 && strcmp(out, "\xe2\x82\xac") == 0);
    CHECK(wifi_ssid_sanitize("a\xf0\x9f\x93\xb6", out, 5) == 1 && strcmp(out, "a") == 0); // 4 + NUL does not fit after the a

    if (failures) {
        return 1;
    }
    printf("wifi_list_test: ok\n");
    return 0;
}
