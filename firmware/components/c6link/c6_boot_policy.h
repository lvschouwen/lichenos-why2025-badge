#pragma once

#include <stdbool.h>
#include <stddef.h>

// Whether a boot may skip the ROM-bootloader check of the C6 flash (#36). Pure logic, no ESP headers: host-tested in
// host_tests/c6_boot_policy_test.c.

typedef enum {
    C6_RST_SOFTWARE, // esp_restart: OTA reboot, "fw restart", a deliberate system_restart
    C6_RST_OTHER,    // power-on, external reset, panic, any watchdog, brownout, deep sleep, unknown
} c6_reset_kind_t;

typedef enum { C6_PLAN_FULL_CHECK, C6_PLAN_SKIP } c6_plan_t;

// md5 (32 hex digits each) and size of the bootloader, partition table and app. Rejected: a missing or malformed md5.
// The key is lowercase "md5,md5,md5,size,size,size"; it changes when any of the six values does.
#define C6_BUNDLE_KEY_MAX 128
bool c6_bundle_key(char const *boot_md5, char const *part_md5, char const *app_md5, unsigned long boot_size,
                   unsigned long part_size, unsigned long app_size, char *out, size_t cap);

// SKIP only after a software reset, with no forced reflash, no "verify next boot" flag and not on a trial image
// (PENDING_VERIFY: a restart there would make the bootloader roll the update back), when the key stored after the last
// successful verify or flash is non-empty and equals the current one. Everything else is a full check. why gets a short
// reason for the log (may be NULL).
c6_plan_t c6_boot_plan(c6_reset_kind_t kind, bool force, bool verify_flag, bool trial_image, char const *stored_key,
                       char const *current_key, char const **why);

// What to persist once the ESP-Hosted attach has been tried. write == false: leave NVS alone. Otherwise store_key says
// whether to store the current bundle key (else erase it) and verify whether to set the "check fully next boot" flag
// (else clear it). The key is only ever stored after a successful attach on a verified bundle.
typedef struct {
    bool write;
    bool store_key;
    bool verify;
} c6_state_write_t;
c6_state_write_t c6_state_after_attach(bool full_path, bool attached, bool image_ok, bool key_valid);

// Restart only after a failed attach on the skip path, and only if the verify flag / key erase really persisted;
// otherwise the next boot would skip again and loop. Everything else carries on (without a link when not attached).
bool c6_restart_after_attach(bool full_path, bool attached, bool state_persisted);
