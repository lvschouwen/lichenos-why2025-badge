// The hardware-touching part of lichen_sys_clock.h, lichen_sys_net.h, lichen_sys_c6.h, lichen_sys_psram.h,
// lichen_sys_boot.h, lichen_sys_keys.h and lichen_sys_led.h (#244, #284): forwards to the clock, Wi-Fi, mDNS, C6 link,
// PSRAM guard, boot progress, keyboard and LED services, which the shell may not include (tools/layers.conf). The pure helpers are in lichen_sys_pure_view.c. The gate's types repeat
// the services' and are asserted equal there.
#include "boot_progress.h"
#include "c6link.h"
#include "keyboard.h"
#include "led.h"
#include "lichen_clock.h"
#include "lichen_mdns.h"
#include "lichen_sys_boot.h"
#include "lichen_sys_c6.h"
#include "lichen_sys_clock.h"
#include "lichen_sys_keys.h"
#include "lichen_sys_led.h"
#include "lichen_sys_net.h"
#include "lichen_sys_psram.h"
#include "log_tap.h"
#include "lora_app.h"
#include "psram_boot.h"
#include "psram_dma_cmd.h"
#include "psram_speed.h"
#include "wifi_service.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MIRROR_ENUM_FIELD(mine, theirs, field)                                                                         \
    _Static_assert(                                                                                                    \
        offsetof(mine, field) == offsetof(theirs, field) &&                                                            \
            sizeof(((mine *)0)->field) == sizeof(((theirs *)0)->field),                                                \
        "the gate's " #mine "." #field " follows " #theirs                                                             \
    )
#define MIRROR_FIELD(mine, theirs, field)                                                                              \
    MIRROR_ENUM_FIELD(mine, theirs, field);                                                                            \
    _Static_assert(                                                                                                    \
        __builtin_types_compatible_p(__typeof__(((mine *)0)->field), __typeof__(((theirs *)0)->field)),                \
        "the gate's " #mine "." #field " has the type of " #theirs                                                     \
    )

_Static_assert(
    sizeof(lichen_clock_sync_t) == sizeof(clock_sync_info_t), "the gate's clock state follows lichen_clock's"
);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, synced);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, last_good_s);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, last_good_mono_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, step_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, step_mono_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, step_span_ms);
MIRROR_ENUM_FIELD(lichen_clock_sync_t, clock_sync_info_t, last_verdict);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, refused);
MIRROR_ENUM_FIELD(lichen_clock_sync_t, clock_sync_info_t, source);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, source_mono_ms);
MIRROR_ENUM_FIELD(lichen_clock_sync_t, clock_sync_info_t, was);
_Static_assert(
    LICHEN_LOG_TAP_LINES == LOG_TAP_LINES && LICHEN_LOG_TAP_LINE_LEN == LOG_TAP_LINE_LEN,
    "the gate's log tap follows lichen_boot's"
);

bool lichen_sys_clock_is_set(void) {
    return clock_is_set();
}

bool lichen_sys_clock_is_approximate(void) {
    return clock_is_approximate();
}

int64_t lichen_sys_clock_build_epoch(void) {
    return clock_build_epoch();
}

lichen_clock_verdict_t lichen_sys_clock_set_manual(struct timeval const *tv, bool force) {
    return (lichen_clock_verdict_t)clock_set_manual(tv, force);
}

lichen_clock_sync_t lichen_sys_clock_sync_info(void) {
    clock_sync_info_t   info = clock_sync_info();
    lichen_clock_sync_t out;
    memcpy(&out, &info, sizeof(out));
    return out;
}

void lichen_sys_clock_source_text(char *out, size_t cap) {
    clock_source_text(out, cap);
}

int64_t lichen_sys_clock_source_age_s(void) {
    return clock_source_age_s();
}

// The scan results are the same struct under two names (asserted in lichen_sys_pure_view.c); the cast is safe because
// the two names are only ever used in different translation units.
bool lichen_sys_wifi_scan(lichen_sys_wifi_ap_t *out, size_t max, size_t *n) {
    return wifi_scan((wifi_ap_t *)out, max, n);
}

bool lichen_sys_wifi_scan_raw(lichen_sys_wifi_ap_t *out, size_t max, size_t *n) {
    return wifi_scan_raw((wifi_ap_t *)out, max, n);
}

lichen_sys_wifi_join_t lichen_sys_wifi_join_state(char *tried, size_t tried_cap, char *kept, size_t kept_cap) {
    return lichen_sys_wifi_join_of((int)wifi_join_state(tried, tried_cap, kept, kept_cap));
}

char const *lichen_sys_mdns_name(void) {
    return lichen_mdns_name();
}

void lichen_sys_boot_view(lichen_boot_view_t *out) {
    boot_view_t view;
    boot_progress_view(&view);
    memcpy(out, &view, sizeof(*out));
}

char const *lichen_sys_boot_label(lichen_boot_step_t step) {
    return boot_progress_label((boot_step_t)step);
}

size_t lichen_sys_log_tap_last(char lines[][LICHEN_LOG_TAP_LINE_LEN], size_t max) {
    return log_tap_last(lines, max);
}

bool lichen_sys_keys_hook_add(key_raw_fn fn, void *arg) {
    return keyboard_raw_add(fn, arg);
}

void lichen_sys_keys_hook_remove(key_raw_fn fn, void *arg) {
    keyboard_raw_remove(fn, arg);
}

void lichen_sys_keys_set_wake(bool (*fn)(void)) {
    keyboard_set_wake_hook(fn);
}

void lichen_sys_keys_set_gate(bool (*fn)(uint8_t key, bool pressed), void (*reset)(void)) {
    keyboard_set_gate_hook(fn, reset);
}

bool lichen_sys_led_enabled(void) {
    return led_enabled();
}

void lichen_sys_led_set_enabled(bool on) {
    led_set_enabled(on);
}

// Wi-Fi, the rest of lichen_sys_net.h: one-for-one forwards, no state of their own
bool lichen_sys_wifi_up(void) {
    return wifi_has_ip(NULL, 0);
}

bool lichen_sys_wifi_ip(char *ip, size_t cap) {
    return wifi_has_ip(ip, cap);
}

bool lichen_sys_wifi_joining(void) {
    return wifi_joining();
}

bool lichen_sys_wifi_has_credentials(void) {
    return wifi_has_credentials();
}

bool lichen_sys_wifi_rssi(int8_t *out) {
    return wifi_rssi(out);
}

bool lichen_sys_wifi_join(char const *ssid, char const *pass) {
    return wifi_set_credentials(ssid, pass);
}

bool lichen_sys_wifi_forget(char const *ssid) {
    return wifi_forget_network(ssid);
}

size_t lichen_sys_wifi_saved_names(char (*out)[33], size_t max) {
    return wifi_saved_names(out, max);
}

// The C6 link and the LoRa counts (lichen_sys_c6.h)
bool lichen_sys_c6_up(void) {
    return c6link_up();
}

bool lichen_sys_c6_trouble(char *out, size_t cap) {
    return c6link_trouble(out, cap);
}

uint32_t lichen_sys_c6_restarts(char *reason, size_t cap) {
    static c6_status_t st; // the reason is long; only the sys_notices task calls this
    c6link_status(&st);
    snprintf(reason, cap, "%s", st.reason);
    return st.restarts;
}

bool lichen_sys_lora_up(void) {
    return lora_up() && c6link_up();
}

void lichen_sys_lora_counts(uint32_t *rx, uint32_t *tx) {
    lora_get_counts(rx, tx);
}

// The PSRAM guard (lichen_sys_psram.h)
unsigned lichen_sys_psram_mhz(void) {
    return psram_speed_running();
}

bool lichen_sys_psram_speed_supported(void) {
    return psram_speed_supported();
}

bool lichen_sys_psram_speed_choice(unsigned target_mhz) {
    return psram_speed_choice(target_mhz);
}

size_t lichen_sys_psram_speed_value(char *out, size_t cap) {
    return psram_speed_value(out, cap);
}

unsigned lichen_sys_psram_speed_other(void) {
    return psram_speed_other();
}

size_t lichen_sys_psram_tuning_text(char *out, size_t cap) {
    return psram_boot_tuning_text(out, cap);
}

_Static_assert(sizeof(lichen_sys_psram_round_info_t) == sizeof(psram_dma_round_info_t),
               "the gate's round info follows psram's");
_Static_assert((int)LICHEN_SYS_PSRAM_ROUND_PASS == (int)PSRAM_DMA_ROUND_PASS &&
                   (int)LICHEN_SYS_PSRAM_ROUND_FAIL == (int)PSRAM_DMA_ROUND_FAIL &&
                   (int)LICHEN_SYS_PSRAM_ROUND_BUSY == (int)PSRAM_DMA_ROUND_BUSY &&
                   (int)LICHEN_SYS_PSRAM_ROUND_NOT_RUN == (int)PSRAM_DMA_ROUND_NOT_RUN,
               "the gate's round result follows psram_dma_round_t");
MIRROR_FIELD(lichen_sys_psram_round_info_t, psram_dma_round_info_t, bad_bytes);
MIRROR_FIELD(lichen_sys_psram_round_info_t, psram_dma_round_info_t, errors);
MIRROR_FIELD(lichen_sys_psram_round_info_t, psram_dma_round_info_t, sd_skipped);

lichen_sys_psram_round_t lichen_sys_psram_round(lichen_sys_psram_round_info_t *info) {
    psram_dma_round_info_t raw;
    psram_dma_round_t      r = psram_dma_quick_round(info ? &raw : NULL);
    if (info) {
        info->bad_bytes  = raw.bad_bytes;
        info->errors     = raw.errors;
        info->sd_skipped = raw.sd_skipped;
    }
    return (lichen_sys_psram_round_t)r;
}
