#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Which NVS boot path an app image was built with (#116), readable from the other OTA slot without running it. Before
// the HMAC key is burned both slots must hold a LICHEN_NVS_ENCRYPT=y image: a rollback to a plain image runs
// nvs_flash_init() on the encrypted partition, which takes every entry for damaged and erases it.
//
// lichen_nvs.c places one nvs_mark_t in IDF's custom app description section (.rodata_custom_desc), which the linker
// puts right after esp_app_desc_t, so it sits at a fixed offset in every image: NVS_MARK_OFFSET from the slot's start.
// Images built before this mark existed have other rodata there and read as "no mark". Pure, host-tested in
// host_tests/nvs_mark_test.c.

// sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t); checked in lichen_nvs.c
#define NVS_MARK_OFFSET (24 + 8 + 256)

#define NVS_MARK_TAG      {'L', 'I', 'C', 'H', 'E', 'N', 'N', 'V'}
#define NVS_MARK_VERSION  1
#define NVS_MARK_F_ENCRYPT 0x01u // built with LICHEN_NVS_ENCRYPT
#define NVS_MARK_F_BURN    0x02u // and with the burn command (LICHEN_NVS_ENCRYPT_BURN_CMD)

typedef struct {
    uint8_t tag[8];
    uint8_t version;
    uint8_t flags;
    uint8_t reserved[2];
} nvs_mark_t;

typedef enum {
    NVS_MARK_NO_APP,       // the slot holds no valid app description
    NVS_MARK_NONE,         // an app without the mark: built before it existed, plain NVS
    NVS_MARK_PLAIN,        // built with LICHEN_NVS_ENCRYPT off
    NVS_MARK_ENCRYPT,      // built with it on
    NVS_MARK_ENCRYPT_BURN, // on, and the burn command compiled in (not an image to keep after the burn)
} nvs_mark_kind_t;

// bytes: what the slot holds at NVS_MARK_OFFSET (len of them, sizeof(nvs_mark_t) needed); has_app from the app
// description check
nvs_mark_kind_t nvs_mark_parse(bool has_app, uint8_t const *bytes, size_t len);
// The image keeps an encrypted NVS readable (opens it with the derived keys instead of plain init)
bool            nvs_mark_encrypts(nvs_mark_kind_t k);
// Both slots: a rollback either way keeps an encrypted NVS
bool            nvs_marks_rollback_safe(nvs_mark_kind_t running, nvs_mark_kind_t other);
char const     *nvs_mark_name(nvs_mark_kind_t k);
