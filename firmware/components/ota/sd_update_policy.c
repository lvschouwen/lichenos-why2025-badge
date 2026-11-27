#include "sd_update_policy.h"

#include "ota_valid.h"

#include <string.h>

#define APP_DESC_OFFSET 0x20 // esp_app_desc_t after the 24-byte image header and the first 8-byte segment header
#define APP_DESC_MAGIC  0xABCD5432u
#define APP_DESC_SHA    0x90 // app_elf_sha256 inside esp_app_desc_t

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
