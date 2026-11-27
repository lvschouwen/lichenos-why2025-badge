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

// Whether the staged C6 bundle could still change what the C6 runs (#420: the on-air list is not switched on then):
// changed this boot (a push or delete; flashed at the next boot), not checked yet this boot, or staged (staged: its key
// could be built) but not proven by the running C6 (IDENT did not match, e.g. after a failed flash). A boot whose C6
// proved the staged bundle is not pending, nor one with no bundle staged (the flasher then leaves the C6 alone).
bool c6_bundle_pending(bool changed, bool checked, bool staged, bool ident_ok);

// The SDIO link clock (#583). IDF's SDIO card init sets exactly SDMMC_FREQ_HIGHSPEED (C6_SDIO_HS_CAP_KHZ) when the card
// took High Speed mode, whatever the host asked for; the C6 runs High Speed up to 50000. After an attach that left
// slot 1 at applied_khz, the clock to set (configured_khz), or 0 to leave it: not in High Speed (20000, the default
// speed, must stay there), nothing applied (no slot), or configured at or below the cap.
#define C6_SDIO_HS_CAP_KHZ 40000
uint32_t c6_sdio_raise_khz(uint32_t configured_khz, int applied_khz);

// The C6 bundle on the SD card (#595): /sd/lichen/update/c6 holds the same six files as /storage/c6. Read once per
// boot, before the flasher plan, from stat and the .md5 texts only (the full md5 runs only for a NEW bundle).
typedef enum {
    C6_CARD_NONE,   // none of the six files: nothing to say
    C6_CARD_HALF,   // some of the six, or a malformed .md5: ignored with a notice, /storage/c6 is used
    C6_CARD_SAME,   // the card holds what /storage/c6 holds (the copy after the last card install): nothing to do
    C6_CARD_FAILED, // this bundle was flashed from the card before and never proven by IDENT under this P4 image
    C6_CARD_NEW,    // differs from /storage/c6: the source once its md5 check passes
} c6_card_t;
// files: how many of the six are there; card_key / storage_key: c6_bundle_key of each ("" when it could not be built);
// failed_before: the failed record stored in NVS is this card bundle's under the running P4 (c6_card_record)
c6_card_t   c6_card_classify(unsigned files, char const *card_key, char const *storage_key, bool failed_before);
char const *c6_card_name(c6_card_t c);
// Whether the flasher takes the card bundle: only a NEW one whose three .bin files match their .md5
bool        c6_card_use(c6_card_t c, bool md5_ok);

// The failed record: the card bundle's key and the running P4's ELF sha256 (hex), so the record holds only for the P4
// image it was made under. A card with a new C6 and a new P4 whose contract changed: the old P4 cannot get IDENT from
// the new C6 and records it failed, the new P4 then gives it one try of its own instead of reflashing the old C6.
#define C6_CARD_RECORD_MAX (C6_BUNDLE_KEY_MAX + 1 + 2 * C6_IDENT_SHA_LEN + 1)
bool c6_card_record(char const *card_key, uint8_t const p4_sha[C6_IDENT_SHA_LEN], char *out, size_t cap);

// After the attach and IDENT, for a boot whose flasher took the card bundle (from_card): COPY it into /storage/c6 once
// IDENT proved it (the no-card rescue: the C6 is the only Wi-Fi way back in), but not while the running P4 is on trial
// (p4_trial: a definite trial): after a contract change a P4 rollback would leave the old P4 with a rescue copy it
// cannot talk to; the first boot after the mark-valid copies it (the card still differs from /storage). RECORD_FAILED
// when the flasher completed with the card bundle on the C6 (on_c6: written this boot or found there already, e.g.
// flashed under another P4 image) and IDENT did not prove it, so the next boot goes back to /storage/c6. Else KEEP: a
// flash that did not complete (a card read error, UART busy) proves nothing, the next boot tries again.
typedef enum { C6_CARD_KEEP, C6_CARD_COPY, C6_CARD_RECORD_FAILED } c6_card_after_t;
c6_card_after_t c6_card_after(bool from_card, bool on_c6, bool ident_ok, bool p4_trial);

// Whether /storage has room for the rescue copy: the three new .bin files and their .md5 next to the old ones (they
// are written whole before any rename, #595 review). LittleFS: 4088 B of each 4096 B block per file, plus one block of
// metadata per file and C6_CARD_COPY_SLACK blocks. free unknown (free_known false): let it try, the write says.
#define C6_CARD_COPY_SLACK 4
bool c6_card_copy_fits(bool free_known, uint64_t free_bytes, unsigned long const bin_sizes[3]);
