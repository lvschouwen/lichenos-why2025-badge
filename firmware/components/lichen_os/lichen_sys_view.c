// The hardware-touching part of lichen_sys_clock.h, lichen_sys_net.h, lichen_sys_boot.h, lichen_sys_keys.h and
// lichen_sys_led.h (#244): forwards to the clock, Wi-Fi, mDNS, boot progress, keyboard and LED services, which the
// shell may not include (tools/layers.conf). The pure helpers are in lichen_sys_pure_view.c. The gate's types repeat the
// services' and are asserted equal there.
#include "lichen_sys_boot.h"
#include "lichen_sys_clock.h"
#include "lichen_sys_keys.h"
#include "lichen_sys_led.h"
#include "lichen_sys_net.h"

#include "boot_progress.h"
#include "keyboard.h"
#include "led.h"
#include "lichen_clock.h"
#include "lichen_mdns.h"
#include "log_tap.h"
#include "wifi_service.h"

#include <stddef.h>
#include <string.h>

#define MIRROR_FIELD(mine, theirs, field)                                                                              \
    _Static_assert(offsetof(mine, field) == offsetof(theirs, field) &&                                                 \
                       sizeof(((mine *)0)->field) == sizeof(((theirs *)0)->field),                                     \
                   "the gate's " #mine "." #field " follows " #theirs)

_Static_assert(sizeof(lichen_clock_sync_t) == sizeof(clock_sync_info_t), "the gate's clock state follows lichen_clock's");
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, synced);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, last_good_s);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, last_good_mono_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, step_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, step_mono_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, step_span_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, last_verdict);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, refused);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, source);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, source_mono_ms);
MIRROR_FIELD(lichen_clock_sync_t, clock_sync_info_t, was);
_Static_assert(LICHEN_SYS_MDNS_NAME_MAX == 16, "the gate's mDNS name size follows lichen_mdns's MDNS_NAME_MAX");
_Static_assert(LICHEN_SYS_KEY_HOOKS_MAX == KEYBOARD_RAW_MAX, "the gate's hook count follows board's");
_Static_assert(LICHEN_LOG_TAP_LINES == LOG_TAP_LINES && LICHEN_LOG_TAP_LINE_LEN == LOG_TAP_LINE_LEN,
               "the gate's log tap follows lichen_boot's");

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
    switch (wifi_join_state(tried, tried_cap, kept, kept_cap)) {
    case WIFI_JOIN_PENDING: return LICHEN_SYS_WIFI_JOIN_PENDING;
    case WIFI_JOIN_TRYING: return LICHEN_SYS_WIFI_JOIN_TRYING;
    case WIFI_JOIN_JOINED: return LICHEN_SYS_WIFI_JOIN_JOINED;
    case WIFI_JOIN_FAILED_KEPT_OLD: return LICHEN_SYS_WIFI_JOIN_FAILED_KEPT_OLD;
    case WIFI_JOIN_NONE:
    default: return LICHEN_SYS_WIFI_JOIN_NONE;
    }
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

// board's hooks take its own event type; the two are the same bytes (asserted in lichen_sys_pure_view.c), so the
// function pointer is passed through. Called on the keyboard task, which only reads the event.
bool lichen_sys_keys_hook_add(lichen_sys_key_fn fn, void *arg) {
    return keyboard_raw_add((key_raw_fn)fn, arg);
}

void lichen_sys_keys_hook_remove(lichen_sys_key_fn fn, void *arg) {
    keyboard_raw_remove((key_raw_fn)fn, arg);
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
