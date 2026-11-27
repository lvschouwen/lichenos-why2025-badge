#include "sd_update_policy.h"

#include "ota_valid.h"

#include <string.h>

#define APP_DESC_OFFSET SDU_APP_DESC_OFFSET
#define APP_DESC_MAGIC  SDU_APP_DESC_MAGIC
#define APP_DESC_SHA    SDU_APP_DESC_SHA

_Static_assert(APP_DESC_OFFSET + APP_DESC_SHA + SDU_SHA_LEN == SDU_HEAD_LEN, "head length");

sdu_card_t sdu_card_from_head(uint8_t const *head, size_t len, uint16_t chip_id, uint8_t elf_sha[SDU_SHA_LEN]) {
    if (!head || len < SDU_HEAD_LEN) {
        return SDU_CARD_UNREADABLE;
    }
    uint8_t const *d     = &head[APP_DESC_OFFSET];
    uint32_t       magic = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    if (!ota_image_header_ok(head, len, chip_id) || magic != APP_DESC_MAGIC) {
        return SDU_CARD_NOT_APP;
    }
    uint8_t any = 0;
    for (int i = 0; i < SDU_SHA_LEN; i++) {
        any |= d[APP_DESC_SHA + i];
    }
    if (!any) {
        return SDU_CARD_NOT_APP; // no ELF sha filled in: nothing to compare with, so never installed blind
    }
    memcpy(elf_sha, &d[APP_DESC_SHA], SDU_SHA_LEN);
    return SDU_CARD_OK;
}

sdu_action_t sdu_decide(sdu_card_t card, bool same_as_running, bool same_as_attempted, bool on_trial) {
    switch (card) {
    case SDU_CARD_ABSENT: return SDU_SKIP_ABSENT;
    case SDU_CARD_UNREADABLE: return SDU_SKIP_UNREADABLE;
    case SDU_CARD_NOT_APP: return SDU_SKIP_NOT_APP;
    case SDU_CARD_OK: break;
    default: return SDU_SKIP_UNREADABLE;
    }
    if (same_as_running) {
        return SDU_SKIP_SAME; // first, so the boot right after a card install (still on trial) says nothing
    }
    if (same_as_attempted) {
        return SDU_SKIP_TRIED;
    }
    return on_trial ? SDU_SKIP_TRIAL : SDU_INSTALL;
}

char const *sdu_action_name(sdu_action_t a) {
    switch (a) {
    case SDU_INSTALL: return "install";
    case SDU_SKIP_ABSENT: return "no image on the card";
    case SDU_SKIP_UNREADABLE: return "unreadable";
    case SDU_SKIP_NOT_APP: return "not a badge image";
    case SDU_SKIP_SAME: return "already running";
    case SDU_SKIP_TRIED: return "tried before, rolled back";
    case SDU_SKIP_TRIAL: return "running image on trial";
    default: return "?";
    }
}

bool sdu_action_notice(sdu_action_t a, bool told) {
    return a == SDU_SKIP_UNREADABLE || a == SDU_SKIP_NOT_APP || (a == SDU_SKIP_TRIED && !told);
}

bool sdu_result_records(sdu_result_t r) {
    return r == SDU_FAIL_IMAGE || r == SDU_FAIL_READBACK;
}

char const *sdu_result_text(sdu_result_t r) {
    switch (r) {
    case SDU_DONE: return "installed";
    case SDU_FAIL_READ: return "badge_fw.bin could not be read from the card";
    case SDU_FAIL_SHA: return "badge_fw.bin does not match badge_fw.bin.sha256";
    case SDU_FAIL_NO_SLOT: return "no room to write it (slot, size or memory)";
    case SDU_FAIL_WRITE: return "writing the image failed";
    case SDU_FAIL_IMAGE: return "the image failed its check after the copy; it is not tried again";
    case SDU_FAIL_READBACK: return "the written image did not read back the same; it is not tried again";
    case SDU_FAIL_RECORD: return "the attempt could not be recorded";
    case SDU_FAIL_BOOT: return "the new image could not be made the boot slot";
    default: return "?";
    }
}

char const *sdu_tried_text(uint8_t flags) {
    return flags & SDU_REC_FAILED ? "This image failed its check after the copy before; change the file to try again"
                                  : "This image was tried from the card before; change the file to try again";
}

sdu_step_t sdu_step_of(sdu_action_t a, sdu_result_t r) {
    switch (a) {
    case SDU_SKIP_SAME: return SDU_STEP_DONE;
    case SDU_SKIP_UNREADABLE:
    case SDU_SKIP_NOT_APP: return SDU_STEP_FAILED;
    case SDU_INSTALL: return r == SDU_DONE ? SDU_STEP_DONE : SDU_STEP_FAILED;
    case SDU_SKIP_ABSENT:
    case SDU_SKIP_TRIED:
    case SDU_SKIP_TRIAL:
    default: return SDU_STEP_SKIPPED;
    }
}

uint8_t sdu_flags_marked_wifi(uint8_t flags) {
    return (uint8_t)(flags | SDU_REC_WIFI);
}

bool sdu_trial_without_wifi(uint8_t flags) {
    return !(flags & SDU_REC_WIFI);
}

uint8_t sdu_flags_after(uint8_t old, sdu_result_t r) {
    if (r == SDU_DONE) {
        return 0;
    }
    if (sdu_result_records(r)) {
        return (uint8_t)(SDU_REC_FAILED | SDU_REC_TOLD | (old & SDU_REC_WIFI));
    }
    return old;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool sdu_sha256_text_ok(char const *text, size_t len, uint8_t const digest[SDU_SHA_LEN]) {
    if (!text || !digest || len < 2 * SDU_SHA_LEN) {
        return false;
    }
    for (int i = 0; i < SDU_SHA_LEN; i++) {
        int hi = hex_digit(text[2 * i]);
        int lo = hex_digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0 || (uint8_t)(hi << 4 | lo) != digest[i]) {
            return false;
        }
    }
    if (len == 2 * SDU_SHA_LEN) {
        return true;
    }
    char next = text[2 * SDU_SHA_LEN];
    return next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '\0'; // not a 65th hex digit
}
