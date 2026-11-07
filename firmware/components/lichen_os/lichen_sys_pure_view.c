// The ESP-free part of lichen_sys_clock.h, lichen_sys_net.h, lichen_sys_boot.h and lichen_sys_keys.h (#244): the pure
// helpers the shell's views call, one-for-one over lichen_clock's clock_check.c / clock_local.c, lichen_net's
// wifi_list.c / wifi_analyser.c and lichen_boot's boot_text.c / log_clean.c. Kept apart from the glue so
// host_tests/lichen_sys_pure_view_test.c links it. The gate's types repeat the components' (it cannot include them);
// the asserts below keep them equal, so the conversions are plain casts and copies.
#include "lichen_sys_boot.h"
#include "lichen_sys_clock.h"
#include "lichen_sys_keys.h"
#include "lichen_sys_net.h"

#include "boot_text.h"
#include "clock_check.h"
#include "clock_local.h"
#include "key_raw.h"
#include "log_clean.h"
#include "wifi_analyser.h"
#include "wifi_list.h"

#include <stddef.h>
#include <string.h>

// A mirror field matches when it sits at the same offset with the same size (and so the same type width)
#define MIRROR_FIELD(mine, theirs, field)                                                                              \
    _Static_assert(offsetof(mine, field) == offsetof(theirs, field) &&                                                 \
                       sizeof(((mine *)0)->field) == sizeof(((theirs *)0)->field),                                     \
                   "the gate's " #mine "." #field " follows " #theirs)
#define MIRROR_ENUM(mine, theirs) _Static_assert((int)(mine) == (int)(theirs), "the gate's " #mine " follows " #theirs)

MIRROR_ENUM(LICHEN_CLOCK_OK, CLOCK_OK);
MIRROR_ENUM(LICHEN_CLOCK_TOO_OLD, CLOCK_TOO_OLD);
MIRROR_ENUM(LICHEN_CLOCK_TOO_NEW, CLOCK_TOO_NEW);
MIRROR_ENUM(LICHEN_CLOCK_BIG_STEP, CLOCK_BIG_STEP);
MIRROR_ENUM(LICHEN_CLOCK_RECOVERED, CLOCK_RECOVERED);
MIRROR_ENUM(LICHEN_CLOCK_OUTRANKED, CLOCK_OUTRANKED);
MIRROR_ENUM(LICHEN_CLOCK_NOT_FORWARD, CLOCK_NOT_FORWARD);
MIRROR_ENUM(LICHEN_CLOCK_NO_SOURCE, CLOCK_NO_SOURCE);
MIRROR_ENUM(LICHEN_CLOCK_SRC_NONE, CLOCK_SRC_NONE);
MIRROR_ENUM(LICHEN_CLOCK_SRC_RTC, CLOCK_SRC_RTC);
MIRROR_ENUM(LICHEN_CLOCK_SRC_APP, CLOCK_SRC_APP);
MIRROR_ENUM(LICHEN_CLOCK_SRC_MANUAL, CLOCK_SRC_MANUAL);
MIRROR_ENUM(LICHEN_CLOCK_SRC_SNTP, CLOCK_SRC_SNTP);
MIRROR_ENUM(LICHEN_CLOCK_LOCAL_OK, CLOCK_LOCAL_OK);
MIRROR_ENUM(LICHEN_CLOCK_LOCAL_GAP, CLOCK_LOCAL_GAP);
MIRROR_ENUM(LICHEN_CLOCK_LOCAL_TWICE, CLOCK_LOCAL_TWICE);
MIRROR_ENUM(LICHEN_CLOCK_LOCAL_BAD_DATE, CLOCK_LOCAL_BAD_DATE);

_Static_assert(LICHEN_SYS_WIFI_SSID_MAX + 1 == sizeof(((wifi_ap_t *)0)->ssid), "the gate's SSID size follows lichen_net's");
_Static_assert(sizeof(lichen_sys_wifi_ap_t) == sizeof(wifi_ap_t), "the gate's AP follows lichen_net's");
MIRROR_FIELD(lichen_sys_wifi_ap_t, wifi_ap_t, ssid);
MIRROR_FIELD(lichen_sys_wifi_ap_t, wifi_ap_t, rssi);
MIRROR_FIELD(lichen_sys_wifi_ap_t, wifi_ap_t, open);
MIRROR_FIELD(lichen_sys_wifi_ap_t, wifi_ap_t, channel);
_Static_assert(LICHEN_SYS_WIFI_AN_CHANNELS == WIFI_AN_CHANNELS && LICHEN_SYS_WIFI_METER_LEN == WIFI_METER_LEN &&
                   LICHEN_SYS_WIFI_RSSI_NONE == WIFI_RSSI_NONE && LICHEN_SYS_WIFI_AN_SCAN_GAP_MS == WIFI_AN_SCAN_GAP_MS,
               "the gate's analyser sizes follow lichen_net's");
_Static_assert(sizeof(lichen_sys_wifi_chan_stats_t) == sizeof(wifi_chan_stats_t),
               "the gate's channel stats follow lichen_net's");
MIRROR_FIELD(lichen_sys_wifi_chan_stats_t, wifi_chan_stats_t, count);
MIRROR_FIELD(lichen_sys_wifi_chan_stats_t, wifi_chan_stats_t, best);
_Static_assert(sizeof(lichen_sys_wifi_meter_t) == sizeof(wifi_meter_t), "the gate's meter follows lichen_net's");
MIRROR_FIELD(lichen_sys_wifi_meter_t, wifi_meter_t, v);
MIRROR_FIELD(lichen_sys_wifi_meter_t, wifi_meter_t, n);
MIRROR_FIELD(lichen_sys_wifi_meter_t, wifi_meter_t, head);

MIRROR_ENUM(LICHEN_BOOT_STORAGE, BOOT_STORAGE);
MIRROR_ENUM(LICHEN_BOOT_DISPLAY, BOOT_DISPLAY);
MIRROR_ENUM(LICHEN_BOOT_PSRAM, BOOT_PSRAM);
MIRROR_ENUM(LICHEN_BOOT_C6, BOOT_C6);
MIRROR_ENUM(LICHEN_BOOT_WIFI, BOOT_WIFI);
MIRROR_ENUM(LICHEN_BOOT_CLOCK, BOOT_CLOCK);
MIRROR_ENUM(LICHEN_BOOT_LORA, BOOT_LORA);
MIRROR_ENUM(LICHEN_BOOT_APPS, BOOT_APPS);
MIRROR_ENUM(LICHEN_BOOT_STEP_COUNT, BOOT_STEP_COUNT);
MIRROR_ENUM(LICHEN_STEP_PENDING, STEP_PENDING);
MIRROR_ENUM(LICHEN_STEP_ACTIVE, STEP_ACTIVE);
MIRROR_ENUM(LICHEN_STEP_DONE, STEP_DONE);
MIRROR_ENUM(LICHEN_STEP_FAILED, STEP_FAILED);
MIRROR_ENUM(LICHEN_STEP_SKIPPED, STEP_SKIPPED);
_Static_assert(LICHEN_BOOT_REASON_LEN == BOOT_REASON_LEN, "the gate's boot reason size follows lichen_boot's");
_Static_assert(sizeof(lichen_boot_view_t) == sizeof(boot_view_t), "the gate's boot view follows lichen_boot's");
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, state);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, pct);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, current);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, reason);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, began);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, ended);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, begin_ms);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, end_ms);
MIRROR_FIELD(lichen_boot_view_t, boot_view_t, generation);

_Static_assert(sizeof(lichen_sys_key_event_t) == sizeof(key_raw_event_t), "the gate's key event follows board's");
MIRROR_FIELD(lichen_sys_key_event_t, key_raw_event_t, code);
MIRROR_FIELD(lichen_sys_key_event_t, key_raw_event_t, pressed);
MIRROR_FIELD(lichen_sys_key_event_t, key_raw_event_t, test_open);
MIRROR_FIELD(lichen_sys_key_event_t, key_raw_event_t, shift);

bool lichen_sys_clock_in_window(int64_t t, int64_t build_epoch) {
    return clock_in_window(t, build_epoch);
}

bool lichen_sys_clock_accepted(lichen_clock_verdict_t v) {
    return clock_accepted((clock_verdict_t)v);
}

char const *lichen_sys_clock_verdict_text(lichen_clock_verdict_t v) {
    return clock_verdict_text((clock_verdict_t)v);
}

int lichen_sys_clock_days_in_month(int year, int month) {
    return clock_days_in_month(year, month);
}

lichen_clock_local_t lichen_sys_clock_local_to_utc(int year, int month, int day, int hour, int minute, int64_t *utc) {
    return (lichen_clock_local_t)clock_local_to_utc(year, month, day, hour, minute, utc);
}

char const *lichen_sys_clock_local_hint(lichen_clock_local_t how) {
    return clock_local_hint((clock_local_t)how);
}

char const *lichen_sys_clock_back_hint(int64_t now, int64_t set_to) {
    return clock_back_hint(now, set_to);
}

// The AP lists are the same struct under two names (asserted above): the callee fills the caller's array in place. The
// cast is safe because the two names are only ever used in different translation units, never in one function.
size_t lichen_sys_wifi_list_clean(lichen_sys_wifi_ap_t *aps, size_t n) {
    return wifi_list_clean((wifi_ap_t *)aps, n);
}

size_t lichen_sys_wifi_ssid_sanitize(char const *in, char *out, size_t cap) {
    return wifi_ssid_sanitize(in, out, cap);
}

int lichen_sys_wifi_bars(int8_t rssi) {
    return wifi_bars(rssi);
}

int lichen_sys_wifi_level_pct(int8_t rssi) {
    return wifi_level_pct(rssi);
}

bool lichen_sys_wifi_cred_valid(char const *ssid, char const *pass) {
    return wifi_cred_valid(ssid, pass);
}

uint32_t lichen_sys_wifi_cred_generation(void) {
    return wifi_cred_generation();
}

void lichen_sys_wifi_chan_stats(lichen_sys_wifi_ap_t const *aps, size_t n, lichen_sys_wifi_chan_stats_t *out) {
    wifi_chan_stats((wifi_ap_t const *)aps, n, (wifi_chan_stats_t *)out);
}

void lichen_sys_wifi_meter_clear(lichen_sys_wifi_meter_t *m) {
    wifi_meter_clear((wifi_meter_t *)m);
}

void lichen_sys_wifi_meter_push(lichen_sys_wifi_meter_t *m, int8_t rssi) {
    wifi_meter_push((wifi_meter_t *)m, rssi);
}

int8_t lichen_sys_wifi_meter_at(lichen_sys_wifi_meter_t const *m, size_t i) {
    return wifi_meter_at((wifi_meter_t const *)m, i);
}

bool lichen_sys_wifi_an_lookup(lichen_sys_wifi_ap_t const *aps, size_t n, char const *ssid, lichen_sys_wifi_ap_t *out) {
    return wifi_an_lookup((wifi_ap_t const *)aps, n, ssid, (wifi_ap_t *)out);
}

bool lichen_sys_wifi_an_scan_due(bool ever, uint32_t last_ms, uint32_t now_ms) {
    return wifi_an_scan_due(ever, last_ms, now_ms);
}

char const *lichen_sys_boot_sentence(lichen_boot_step_t step, int pct, char *buf, size_t cap) {
    return boot_step_sentence((boot_step_t)step, pct, buf, cap);
}

int lichen_sys_boot_overall_pct(lichen_boot_view_t const *v) {
    return boot_overall_pct((boot_view_t const *)v);
}

size_t lichen_sys_log_clean(char const *in, size_t len, char *out) {
    return log_clean(in, len, out);
}
