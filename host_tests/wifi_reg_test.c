// Host tests for the Wi-Fi registry keys (#311): the fixed country (NL) as the driver reports it (its three-octet code,
// channels 1-13, policy manual), the regulatory log text, wifi_bandwidth_t -> MHz, the `fw wifi tx|bw|ps` alias replies
// and the wifi.* views (applied range-checked, requested, age, stale and failed notes).
#include "reg_cmd.h"
#include "reg_wifi.h"
#include "wifi_regdom.h"

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

// What the C6 reports after esp_wifi_set_country_code("NL", false)
static wifi_readback_t nl(void) {
    wifi_readback_t r = {.have = true, .at_ms = 1000, .country_ok = true, .proto_ok = true, .bw_ok = true,
                         .tx_ok = true, .ps_ok = true, .cc = {'N', 'L', ' '}, .schan = 1, .nchan = 13,
                         .policy = WIFI_RB_POLICY_MANUAL, .country_max_dbm = 20, .proto = 0x47, .bw_mhz = 20,
                         .tx_raw = 52, .ps = WIFI_RB_PS_NONE};
    return r;
}

static void test_country(void) {
    char out[8];
    CHECK(wifi_country_text((char const[3]){'N', 'L', ' '}, out, sizeof(out)) == 2 && strcmp(out, "NL") == 0);
    CHECK(wifi_country_text((char const[3]){'N', 'L', '\0'}, out, sizeof(out)) == 2 && strcmp(out, "NL") == 0);
    CHECK(wifi_country_text((char const[3]){'0', '1', 'X'}, out, sizeof(out)) == 2 && strcmp(out, "01") == 0);
    CHECK(wifi_country_text((char const[3]){'\0', '\x7f', ' '}, out, sizeof(out)) == 2 && strcmp(out, "??") == 0);
    CHECK(wifi_country_text((char const[3]){'N', 'L', ' '}, out, 2) == 0 && out[0] == '\0');

    wifi_readback_t r = nl();
    CHECK(wifi_regdom_expected(&r));
    char line[80];
    wifi_regdom_text(&r, line, sizeof(line));
    CHECK(strcmp(line, "NL, channels 1-13, max 20 dBm, policy manual") == 0);
    // The driver's default: world safe, 1-11, 802.11d on: not what the firmware set
    r = (wifi_readback_t){.country_ok = true, .cc = {'0', '1', ' '}, .schan = 1, .nchan = 11, .policy = 0,
                          .country_max_dbm = 20};
    CHECK(!wifi_regdom_expected(&r));
    wifi_regdom_text(&r, line, sizeof(line));
    CHECK(strcmp(line, "01, channels 1-11, max 20 dBm, policy auto (802.11d)") == 0);
    r      = nl();
    r.nchan = 11; // NL, but channels 12 and 13 missing
    CHECK(!wifi_regdom_expected(&r));
    r.country_ok = false;
    CHECK(!wifi_regdom_expected(&r));
    wifi_regdom_text(&r, line, sizeof(line));
    CHECK(strcmp(line, "not read") == 0);
}

// #311 review M1/M4: a set since a read makes it unusable; NL as set is never written again
static void test_reuse_and_set(void) {
    wifi_readback_t r = nl(); // read at 1000 ms, set_gen 0
    CHECK(wifi_readback_reusable(&r, 0, 1000));
    CHECK(wifi_readback_reusable(&r, 0, 1000 + WIFI_RB_REUSE_MS - 1));
    CHECK(!wifi_readback_reusable(&r, 0, 1000 + WIFI_RB_REUSE_MS)); // too old
    CHECK(!wifi_readback_reusable(&r, 1, 1100));                     // a setter ran since the read began
    r.have = false;
    CHECK(!wifi_readback_reusable(&r, 0, 1000));

    r = nl();
    CHECK(!wifi_country_needs_set(&r)); // the driver already holds NL, 1-13, manual: no flash write
    r.policy = 0;                       // NL but 802.11d on
    CHECK(wifi_country_needs_set(&r));
    r = (wifi_readback_t){.country_ok = true, .cc = {'0', '1', ' '}, .schan = 1, .nchan = 11, .policy = 0};
    CHECK(wifi_country_needs_set(&r)); // the driver's default (also after esp_wifi_restore)
    r = (wifi_readback_t){0};          // could not read: set
    CHECK(wifi_country_needs_set(&r));
}

static void test_bw(void) {
    CHECK(wifi_bw_to_mhz(1) == 20);
    CHECK(wifi_bw_to_mhz(2) == 40);
    CHECK(wifi_bw_to_mhz(3) == 80);
    CHECK(wifi_bw_to_mhz(4) == 160 && wifi_bw_to_mhz(5) == 160);
    CHECK(wifi_bw_to_mhz(0) == 0 && wifi_bw_to_mhz(9) == 0);
}

static void test_alias_replies(void) {
    char            line[120];
    wifi_readback_t r = nl();
    r.req_tx_raw      = 52;
    wifi_tx_reply(&r, line, sizeof(line));
    CHECK(strcmp(line, "wifi: max TX power 13.00 dBm applied (set; not saved)") == 0);
    r.req_tx_raw = 0;
    r.tx_raw     = 78;
    wifi_tx_reply(&r, line, sizeof(line));
    CHECK(strcmp(line, "wifi: max TX power 19.50 dBm applied (default; not saved)") == 0);
    r.tx_ok = false;
    wifi_tx_reply(&r, line, sizeof(line));
    CHECK(strcmp(line, "wifi: max TX power default, applied not readable (no C6 link?)") == 0);

    r = nl();
    wifi_bw_reply(&r, 40, true, line, sizeof(line));
    CHECK(strcmp(line, "wifi: bandwidth 40 MHz asked, 20 MHz applied now (reconnecting to apply, not saved)") == 0);
    wifi_bw_reply(&r, 40, false, line, sizeof(line));
    CHECK(strcmp(line, "wifi: bandwidth 40 MHz asked, 20 MHz applied now (busy, try again)") == 0);
    wifi_bw_reply(&r, 0, false, line, sizeof(line));
    CHECK(strcmp(line, "wifi: bandwidth 20 MHz applied now (fw wifi bw 20 | 40, not saved)") == 0);
    r.bw_ok = false;
    wifi_bw_reply(&r, 0, false, line, sizeof(line));
    CHECK(strcmp(line, "wifi: bandwidth applied not readable (fw wifi bw 20 | 40, not saved)") == 0);

    r = nl();
    wifi_ps_reply(&r, line, sizeof(line));
    CHECK(strcmp(line, "wifi: power save none applied (fw wifi ps none | min, not saved)") == 0);
    r.ps = WIFI_RB_PS_MIN;
    wifi_ps_reply(&r, line, sizeof(line));
    CHECK(strcmp(line, "wifi: power save min applied (fw wifi ps none | min, not saved)") == 0);
    r.ps_ok = false;
    wifi_ps_reply(&r, line, sizeof(line));
    CHECK(strcmp(line, "wifi: power save not readable (no C6 link?)") == 0);
}

static char const *const ps_names[] = {"none", "min"};
static reg_entry_t const country    = {.id = REG_ID_WIFI_COUNTRY, .type = REG_T_STR, .flags = REG_F_RO | REG_F_TX,
                                       .name = "wifi.country", .min = 2, .max = 2};
static reg_entry_t const txpwr      = {.id = REG_ID_WIFI_TXPWR, .type = REG_T_I16, .flags = REG_F_RO | REG_F_TX,
                                       .name = "wifi.txpwr", .unit = "qdBm", .min = -128, .max = 127};
static reg_entry_t const ps         = {.id = REG_ID_WIFI_PS, .type = REG_T_ENUM, .name = "wifi.ps", .min = 0, .max = 1,
                                       .enum_names = ps_names};
static reg_entry_t const bw         = {.id = REG_ID_WIFI_BW, .type = REG_T_U8, .flags = REG_F_RO | REG_F_TX,
                                       .name = "wifi.bw", .unit = "MHz"};

static void test_views(void) {
    wifi_readback_t r = nl();
    reg_view_t      v;
    reg_wifi_view(&country, &r, true, true, 1500, &v);
    CHECK(v.has_applied && strcmp(v.applied.s, "NL") == 0 && v.note == RV_NONE);
    CHECK(v.has_requested && strcmp(v.requested.s, "NL") == 0);
    CHECK(v.has_age && v.age_ms == 500);
    reg_wifi_view(&txpwr, &r, true, true, 1500, &v);
    CHECK(v.has_applied && v.applied.i == 52 && !v.has_requested); // 0 asked = the driver's own choice
    r.req_tx_raw = 34;
    reg_wifi_view(&txpwr, &r, false, false, 1500, &v);
    CHECK(v.has_requested && v.requested.i == 34 && v.note == RV_NONE); // the LVGL task's cache: not stale
    reg_wifi_view(&txpwr, &r, false, true, 1500, &v);
    CHECK(v.has_applied && v.note == RV_STALE); // a console ask that got nothing: the read before, said so
    r.ps = 2;                                    // max modem sleep: not a name of the key
    reg_wifi_view(&ps, &r, true, true, 1500, &v);
    CHECK(!v.has_applied && v.note == RV_FAILED && v.has_requested && v.requested.u == WIFI_RB_PS_NONE);
    r.bw_mhz = 0; // a bandwidth this build does not name
    reg_wifi_view(&bw, &r, true, true, 1500, &v);
    CHECK(!v.has_applied && v.note == RV_FAILED);
    r = (wifi_readback_t){0}; // never read since boot
    reg_wifi_view(&country, &r, false, true, 1500, &v);
    CHECK(!v.has_applied && !v.has_age && v.note == RV_NO_LINK);

    char line[160];
    r = nl();
    reg_wifi_view(&country, &r, true, true, 1500, &v);
    reg_cmd_row(&country, &v, line, sizeof(line));
    CHECK(strncmp(line, "wifi.country", 12) == 0 && strstr(line, "NL") != NULL && strstr(line, "0.5 s") != NULL);
}

int main(void) {
    test_country();
    test_reuse_and_set();
    test_bw();
    test_alias_replies();
    test_views();
    if (failures) {
        printf("wifi_reg_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("wifi_reg_test: ok\n");
    return 0;
}
