#include "nvs_image.h"

#include <stdlib.h>
#include <string.h>

#define PAGE_UNINIT      0xFFFFFFFFu
#define PAGE_CORRUPT     0x00000000u
#define TABLE_OFFSET     32
#define ENTRY_DATA_START 64
#define ENTRY_SIZE       32
#define ENTRY_COUNT      126
#define ENTRY_WRITTEN    2u // 0b10; empty is 0b11, erased 0b00

// Same CRC as esp_rom_crc32_le(), which NVS uses: the state goes in and out inverted, so calls chain
static uint32_t crc32_le(uint32_t crc, uint8_t const *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t rd32(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Item: nsIndex, datatype, span, chunkIndex (4), crc32 (4), key (16), data (8); the CRC skips itself
static bool item_crc_ok(uint8_t const *it) {
    uint32_t c = crc32_le(0xFFFFFFFFu, it, 4);
    c          = crc32_le(c, it + 8, 24);
    return c == rd32(it + 4);
}

void nvs_image_tally_page(nvs_image_tally_t *t, uint8_t const *page) {
    uint32_t state = rd32(page);
    if (state == PAGE_UNINIT || state == PAGE_CORRUPT) {
        return;
    }
    for (unsigned i = 0; i < ENTRY_COUNT; i++) {
        uint32_t word = rd32(page + TABLE_OFFSET + 4 * (i / 16));
        if (((word >> ((i % 16) * 2)) & 3u) != ENTRY_WRITTEN) {
            continue;
        }
        // The first written entry of a page is an item header: a span's data entries follow their header
        if (item_crc_ok(page + ENTRY_DATA_START + i * ENTRY_SIZE)) {
            t->plain++;
        } else {
            t->bad++;
        }
        return;
    }
}

nvs_image_kind_t nvs_image_tally_kind(nvs_image_tally_t const *t) {
    if (t->plain == 0 && t->bad == 0) {
        return NVS_IMG_EMPTY;
    }
    return t->plain > t->bad ? NVS_IMG_PLAIN : NVS_IMG_ENCRYPTED;
}

nvs_action_t nvs_image_decide(bool enabled, bool key_burned, nvs_image_kind_t kind) {
    if (!enabled || !key_burned) {
        return NVS_ACT_PLAIN_INIT;
    }
    return kind == NVS_IMG_PLAIN ? NVS_ACT_MIGRATE : NVS_ACT_SECURE_INIT;
}

bool nvs_image_keys_ok(uint8_t const eky[32], uint8_t const tky[32]) {
    uint8_t zero[32] = {0};
    return memcmp(eky, zero, 32) != 0 && memcmp(tky, zero, 32) != 0 && memcmp(eky, tky, 32) != 0;
}

bool nvs_snap_add(nvs_snap_t *s, char const *ns, char const *key, uint8_t type, uint64_t num, void const *data,
                  size_t len) {
    if (strlen(ns) >= NVS_SNAP_NAME_MAX || strlen(key) >= NVS_SNAP_NAME_MAX) {
        return false;
    }
    if (s->n == s->cap) {
        size_t            cap = s->cap ? s->cap * 2 : 32;
        nvs_snap_entry_t *e   = realloc(s->e, cap * sizeof(*e));
        if (!e) {
            return false;
        }
        s->e   = e;
        s->cap = cap;
    }
    nvs_snap_entry_t *x = &s->e[s->n];
    memset(x, 0, sizeof(*x));
    strcpy(x->ns, ns);
    strcpy(x->key, key);
    x->type = type;
    x->num  = num;
    if (data) {
        x->data = malloc(len ? len : 1);
        if (!x->data) {
            return false;
        }
        memcpy(x->data, data, len);
        x->len = len;
    }
    s->n++;
    return true;
}

void nvs_snap_zero(nvs_snap_t *s) {
    for (size_t i = 0; i < s->n; i++) {
        if (s->e[i].data) {
            // volatile: the compiler may not drop the stores because the block is freed next
            volatile uint8_t *p = s->e[i].data;
            for (size_t k = 0; k < s->e[i].len; k++) {
                p[k] = 0;
            }
        }
        s->e[i].num = 0;
    }
}

void nvs_snap_wipe(nvs_snap_t *s) {
    nvs_snap_zero(s);
    for (size_t i = 0; i < s->n; i++) {
        free(s->e[i].data);
    }
    free(s->e);
    *s = (nvs_snap_t){0};
}

bool nvs_snap_entry_same(nvs_snap_entry_t const *a, nvs_snap_entry_t const *b) {
    return strcmp(a->ns, b->ns) == 0 && strcmp(a->key, b->key) == 0 && a->type == b->type && a->num == b->num &&
           a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}
