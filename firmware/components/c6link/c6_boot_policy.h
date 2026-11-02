#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

// IDENT (#36): after the attach the P4 asks the running C6 app for its ELF sha256 and compares it with the bundled
// firmware.bin's. Only MATCH proves the C6 runs the bundle; everything else counts as "not proven".
#define C6_IDENT_SHA_LEN 32
typedef enum {
    C6_IDENT_NOT_ASKED,   // no link, so nothing was asked
    C6_IDENT_MATCH,
    C6_IDENT_MISMATCH,    // the C6 runs another app than the bundle holds
    C6_IDENT_NO_ANSWER,   // an image from before IDENT, or a C6 that is not listening (yet)
    C6_IDENT_NO_EXPECTED, // the bundle's .bin has no readable app descriptor
} c6_ident_t;
// running: what the C6 answered (NULL: no answer); expected: from the bundle (NULL: unreadable)
c6_ident_t  c6_ident_check(uint8_t const *running, uint8_t const *expected);
char const *c6_ident_name(c6_ident_t r);

// What to persist once the ESP-Hosted attach and IDENT have been tried. write == false: leave NVS alone. Otherwise
// store_key says whether to store the current bundle key (else erase it) and verify whether to set the "check fully next
// boot" flag (else clear it). The key is only ever stored after a successful attach on a verified bundle whose app the
// running C6 proved (ident_ok), so a boot only skips when the full boot before it saw IDENT match.
typedef struct {
    bool write;
    bool store_key;
    bool verify;
} c6_state_write_t;
c6_state_write_t c6_state_after_attach(bool full_path, bool attached, bool ident_ok, bool image_ok, bool key_valid);

// Restart only after a skip that lost its bet (no attach, or IDENT not proven), and only if the verify flag / key erase
// really persisted; otherwise the next boot would skip again and loop. The full path never restarts; everything else
// carries on (without a link when not attached).
bool c6_restart_after_attach(bool full_path, bool attached, bool ident_ok, bool state_persisted);
