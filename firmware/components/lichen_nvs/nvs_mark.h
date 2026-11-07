#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Which NVS boot path an app image was built with (#116), readable from the other OTA slot without running it. The
// HMAC key may be burned only once both slots hold an image that (a) checks the eFuse before any NVS init (the guard:
// a plain nvs_flash_init() on the encrypted partition takes every entry for damaged and erases it) and (b) is built
// with LICHEN_NVS_ENCRYPT=y, so a rollback still reads the settings.
//
// lichen_nvs.c places one nvs_mark_t in IDF's custom app description section (.rodata_custom_desc), which the linker
// puts right after esp_app_desc_t, so it sits at a fixed offset in every image: NVS_MARK_OFFSET from the slot's start.
// Images built before this mark existed have other rodata there and read as "no mark", hence unguarded. Pure,
// host-tested in host_tests/nvs_mark_test.c.

// sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t); checked in lichen_nvs.c
#define NVS_MARK_OFFSET (24 + 8 + 256)

#define NVS_MARK_TAG       {'L', 'I', 'C', 'H', 'E', 'N', 'N', 'V'}
#define NVS_MARK_VERSION   1
#define NVS_MARK_F_ENCRYPT 0x01u // built with LICHEN_NVS_ENCRYPT
#define NVS_MARK_F_BURN    0x02u // and with the burn command (LICHEN_NVS_ENCRYPT_BURN_CMD)
#define NVS_MARK_F_GUARD   0x04u // reads the eFuse key purpose before any NVS init and never plain-inits on a burned chip

typedef struct {
    uint8_t tag[8];
    uint8_t version;
    uint8_t flags;
    uint8_t reserved[2];
} nvs_mark_t;

typedef struct {
    bool app;     // the slot holds a valid app description
    bool marked;  // and the mark after it
    bool encrypt; // the flags, all false without a mark
    bool burn;
    bool guard;
} nvs_mark_info_t;

typedef enum {
    NVS_SLOTS_UNSAFE,  // a slot without the guard (or no app): a rollback there can erase an encrypted NVS
    NVS_SLOTS_GUARDED, // both guarded, not both encrypt: a rollback leaves NVS down but intact
    NVS_SLOTS_SAFE,    // both guarded encrypt images: the precondition for the burn
} nvs_slots_verdict_t;

// bytes: what the slot holds at NVS_MARK_OFFSET (len of them, sizeof(nvs_mark_t) needed); has_app from the app
// description check
nvs_mark_info_t     nvs_mark_parse(bool has_app, uint8_t const *bytes, size_t len);
nvs_slots_verdict_t nvs_slots_verdict(nvs_mark_info_t running, nvs_mark_info_t other);
// "no app", "no mark (unguarded)", "plain, guard", "encrypt, guard", "encrypt + burn command, guard", ...
void                nvs_mark_describe(nvs_mark_info_t m, char *out, size_t cap);
