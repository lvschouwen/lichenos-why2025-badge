// The P4 image in the SD card's update folder (#595): whether this boot installs it. Pure, no ESP headers: host-tested
// in host_tests/sd_update_policy_test.c. The glue is ota_sd_update.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDU_SHA_LEN 32

// What the first bytes of the card's badge_fw.bin say
typedef enum {
    SDU_CARD_ABSENT,     // no card mounted, or no badge_fw.bin on it
    SDU_CARD_UNREADABLE, // there, but its first bytes could not be read
    SDU_CARD_NOT_APP,    // not an app image for this chip, or no app descriptor with an ELF sha256
    SDU_CARD_OK,
} sdu_card_t;

// Where esp_app_desc_t sits in an app image (after the 24-byte esp_image_header_t and the first 8-byte segment header)
// and its ELF sha256 inside it; ota_sd_update.c pins both to the IDF structs
#define SDU_APP_DESC_OFFSET 0x20
#define SDU_APP_DESC_SHA    0x90
#define SDU_APP_DESC_MAGIC  0xABCD5432u

// The image header (magic, chip) and the esp_app_desc_t at offset 32: SDU_CARD_OK with the ELF sha256 in elf_sha, else
// UNREADABLE (fewer than SDU_HEAD_LEN bytes) or NOT_APP. Needs the first SDU_HEAD_LEN bytes.
#define SDU_HEAD_LEN 0xD0
sdu_card_t sdu_card_from_head(uint8_t const *head, size_t len, uint16_t chip_id, uint8_t elf_sha[SDU_SHA_LEN]);

typedef enum {
    SDU_INSTALL,
    SDU_SKIP_ABSENT,     // nothing on the card: the usual boot, not worth a word
    SDU_SKIP_UNREADABLE, // a notice: the file is there but cannot be read
    SDU_SKIP_NOT_APP,    // a notice: not a badge image
    SDU_SKIP_SAME,       // the card holds the running image
    SDU_SKIP_TRIED,      // installed from the card before and the badge came back on the old one (rollback guard, D1)
    SDU_SKIP_TRIAL,      // the running image is on trial: esp_ota_begin refuses; the boot after its mark-valid decides
} sdu_action_t;

// D1 of #595: install only an image that differs from the running one and was not tried before. same_as_running: the
// card's ELF sha256 is the running app's; same_as_attempted: it is the one recorded before the last card install;
// on_trial: the running image is definitely on trial (blank OTA data after a USB flash is not, or a USB-flashed badge
// would never install from the card).
sdu_action_t sdu_decide(sdu_card_t card, bool same_as_running, bool same_as_attempted, bool on_trial);
char const  *sdu_action_name(sdu_action_t a);
// Whether a skip raises a notice: an unreadable or foreign file always (the card needs fixing), a tried image only the
// first time (told: that notice was raised since the attempt was recorded), the rest never
bool sdu_action_notice(sdu_action_t a, bool told);

// How an install that was let go ended. Only a failure of the image itself, after a whole copy that matched its
// .sha256, is recorded as tried (sdu_result_records): esp_ota_end refused it, or the slot read back different. Those
// would fail the same way on every boot, erasing and rewriting the slot each time; the record makes it one try and
// one notice, until the file on the card changes. A card read that failed (pulled), a .sha256 mismatch (checked before
// anything is erased), a flash write error, no memory or a boot slot that could not be set is not the image's fault
// and is tried again at the next boot.
typedef enum {
    SDU_DONE,          // the boot slot is the new image
    SDU_FAIL_READ,     // the card file could not be read (first pass or while writing)
    SDU_FAIL_SHA,      // the file does not match badge_fw.bin.sha256 (or that file is missing)
    SDU_FAIL_NO_SLOT,  // no inactive slot, the file is larger than it, esp_ota_begin refused, or no memory
    SDU_FAIL_WRITE,    // a flash write failed
    SDU_FAIL_IMAGE,    // esp_ota_end refused the written image
    SDU_FAIL_READBACK, // the slot did not read back as the file
    SDU_FAIL_RECORD,   // the attempt could not be saved, so the boot slot was left alone
    SDU_FAIL_BOOT,     // esp_ota_set_boot_partition failed
} sdu_result_t;
bool        sdu_result_records(sdu_result_t r);
char const *sdu_result_text(sdu_result_t r);

// The attempt record's flags byte (ota_sd_record_t.flags)
#define SDU_REC_TOLD   0x01 // the "not installed again" notice was raised for this image
#define SDU_REC_FAILED 0x02 // it failed its check after the copy (sdu_result_records), rather than being installed
#define SDU_REC_WIFI   0x04 // the update folder was written or deleted in over Wi-Fi since the last install (M3)
// The notice for an image skipped as tried, by the record's flags
char const *sdu_tried_text(uint8_t flags);

// The boot screen's "update" row (BOOT_UPDATE) after the step: nothing there or nothing to do is SKIPPED, a card image
// already running is DONE, a file the badge cannot use or an install that failed is FAILED (its reason stays on screen).
// r is read only for SDU_INSTALL.
typedef enum { SDU_STEP_DONE, SDU_STEP_SKIPPED, SDU_STEP_FAILED } sdu_step_t;
sdu_step_t sdu_step_of(sdu_action_t a, sdu_result_t r);

// Review M3: the provenance of the folder's contents, kept off the card (a file there could be deleted or overwritten
// over Wi-Fi). Every Wi-Fi write or delete under the update folder sets SDU_REC_WIFI in the attempt record (NVS, through
// main's hooks) before it touches the card (sdu_flags_marked_wifi); nothing a Wi-Fi client sends clears it. The install's
// trial needs no Wi-Fi only without it: a card prepared by hand is physical access (like `fw slot other`), an image that
// came over Wi-Fi keeps the Wi-Fi rule (#411), so one that breaks Wi-Fi still rolls back.
uint8_t sdu_flags_marked_wifi(uint8_t flags);
bool    sdu_trial_without_wifi(uint8_t flags);
// The flags of the record an install writes, from the ones it read at the start (old). The attempt before set_boot
// (SDU_DONE) starts afresh: its trial has taken the Wi-Fi bit into account, and only a new Wi-Fi write sets it again.
// A failure of the image (sdu_result_records) is recorded failed and told, keeping the Wi-Fi bit; any other result
// keeps old as it is (the record is not about this image).
uint8_t sdu_flags_after(uint8_t old, sdu_result_t r);

// The text of badge_fw.bin.sha256 (sha256sum's format: 64 hex digits, either case, then the end, white space or the
// file name) names digest
bool sdu_sha256_text_ok(char const *text, size_t len, uint8_t const digest[SDU_SHA_LEN]);
