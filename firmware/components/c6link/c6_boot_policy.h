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

// SKIP only after a software reset, with no forced reflash and no "verify next boot" flag, when the key stored after the
// last successful verify or flash is non-empty and equals the current one. Everything else is a full check. why gets a
// short reason for the log (may be NULL).
c6_plan_t c6_boot_plan(c6_reset_kind_t kind, bool force, bool verify_flag, char const *stored_key, char const *current_key,
                       char const **why);
