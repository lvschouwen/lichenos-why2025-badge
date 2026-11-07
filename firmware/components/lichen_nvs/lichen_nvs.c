#include "lichen_nvs.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "fw_console.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_mark.h"
#include "sdkconfig.h"

#include <stdio.h>

_Static_assert(NVS_BOOT_ERR_NO_FREE_PAGES == ESP_ERR_NVS_NO_FREE_PAGES, "nvs_boot_policy.h out of step with IDF");
_Static_assert(NVS_BOOT_ERR_NEW_VERSION == ESP_ERR_NVS_NEW_VERSION_FOUND, "nvs_boot_policy.h out of step with IDF");
_Static_assert(NVS_MARK_OFFSET == sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t),
               "nvs_mark.h offset out of step with IDF's image format");
_Static_assert(sizeof(nvs_mark_t) == 12, "nvs_mark_t is read from flash byte for byte");

// ---- the image mark (every build) ----------------------------------------------------------------------------------

#if CONFIG_LICHEN_NVS_ENCRYPT
#define MARK_ENCRYPT NVS_MARK_F_ENCRYPT
#else
#define MARK_ENCRYPT 0
#endif
#if CONFIG_LICHEN_NVS_ENCRYPT_BURN_CMD
#define MARK_BURN NVS_MARK_F_BURN
#else
#define MARK_BURN 0
#endif

// Right after esp_app_desc_t in the image (IDF's custom app description section): "fw nvs-slots" reads it from both
// slots. Kept by "-u lichen_nvs_mark" in CMakeLists.txt, since nothing references it.
__attribute__((section(".rodata_custom_desc"), used)) nvs_mark_t const lichen_nvs_mark = {
    .tag     = NVS_MARK_TAG,
    .version = NVS_MARK_VERSION,
    .flags   = MARK_ENCRYPT | MARK_BURN,
};

static nvs_boot_path_t s_path = NVS_BOOT_PATH_PLAIN;
static int             s_boot = -1; // -1 not reported yet, 0 down, 1 up

nvs_boot_path_t lichen_nvs_boot_path(void) {
    return s_path;
}

void lichen_nvs_boot_done(bool up) {
    s_boot = up;
}

// Reads with esp_partition_read, never a mapping (#31): the slot's app description and the mark after it
static nvs_mark_kind_t slot_mark(esp_partition_t const *slot, char *version, size_t cap) {
    esp_app_desc_t desc;
    bool           has_app = slot && esp_ota_get_partition_description(slot, &desc) == ESP_OK;
    snprintf(version, cap, "%s", has_app ? desc.version : "-");
    uint8_t buf[sizeof(nvs_mark_t)];
    bool    read = has_app && esp_partition_read(slot, NVS_MARK_OFFSET, buf, sizeof(buf)) == ESP_OK;
    return nvs_mark_parse(has_app, read ? buf : NULL, read ? sizeof(buf) : 0);
}

static void cmd_nvs_slots(char const *args) {
    (void)args;
    esp_partition_t const *running = esp_ota_get_running_partition();
    esp_partition_t const *other   = esp_ota_get_next_update_partition(NULL);
    char                   ver_r[32], ver_o[32];
    nvs_mark_kind_t        r = slot_mark(running, ver_r, sizeof(ver_r));
    nvs_mark_kind_t        o = slot_mark(other, ver_o, sizeof(ver_o));
    console_reply("nvs-slots: %s (running) %s: %s", running ? running->label : "?", ver_r, nvs_mark_name(r));
    console_reply("nvs-slots: %s %s: %s", other ? other->label : "none", ver_o, nvs_mark_name(o));
    if (nvs_marks_rollback_safe(r, o)) {
        console_reply("nvs-slots: both slots encrypt: a rollback keeps an encrypted NVS");
    } else {
        console_reply("nvs-slots: NOT both encrypt: do not burn the key yet (a rollback would erase an encrypted NVS)");
    }
    if (r == NVS_MARK_ENCRYPT_BURN || o == NVS_MARK_ENCRYPT_BURN) {
        console_reply("nvs-slots: a burn image is on the badge: replace it once the key is burned");
    }
}

static void register_slots(void) {
    static bool done;
    if (!done) {
        done = true;
        console_register("nvs-slots", "nvs-slots (are both OTA slots LICHEN_NVS_ENCRYPT images; read only)",
                         cmd_nvs_slots);
    }
}

#if !CONFIG_LICHEN_NVS_ENCRYPT

esp_err_t lichen_nvs_init(void) {
    register_slots();
    return nvs_flash_init(); // the default build: no eFuse read, no secure init, the plain path
}

#else // CONFIG_LICHEN_NVS_ENCRYPT

#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_log.h"
#include "nvs_erase_os.h"
#include "nvs_image.h"

#include <stdlib.h>
#include <string.h>

static char const TAG[] = "nvs-enc";

#define KEY_ID   ((hmac_key_id_t)CONFIG_LICHEN_NVS_HMAC_KEY_ID)
#define KEY_BLK  ((esp_efuse_block_t)(EFUSE_BLK_KEY0 + CONFIG_LICHEN_NVS_HMAC_KEY_ID))
// IDF's nvs_sec_provider seeds (private header nvs_sec_provider_private.h): the same derivation, so a key burned here
// opens the partition with IDF's own HMAC scheme and tools too
#define EKEY_SEED 0xAEBE5A5Au
#define TKEY_SEED 0xCEDEA5A5u

static bool s_encrypted; // this boot's NVS is the encrypted one

// ---- the eFuse: read only here ------------------------------------------------------------------------------------

// The only eFuse call on the boot path, and it only reads the purpose field. Nothing below writes an eFuse.
static bool key_burned(void) {
    return esp_efuse_get_key_purpose(KEY_BLK) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
}

static bool derive_keys(nvs_sec_cfg_t *cfg) {
    uint32_t eseed[8], tseed[8];
    for (int i = 0; i < 8; i++) {
        eseed[i] = EKEY_SEED;
        tseed[i] = TKEY_SEED;
    }
    memset(cfg, 0, sizeof(*cfg));
    bool ok = esp_hmac_calculate(KEY_ID, eseed, sizeof(eseed), cfg->eky) == ESP_OK &&
              esp_hmac_calculate(KEY_ID, tseed, sizeof(tseed), cfg->tky) == ESP_OK &&
              nvs_image_keys_ok(cfg->eky, cfg->tky);
    if (!ok) {
        memset(cfg, 0, sizeof(*cfg));
    }
    return ok;
}

// ---- what is in the partition -------------------------------------------------------------------------------------

static nvs_image_kind_t scan_once(void) {
    esp_partition_t const *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NVS_DEFAULT_PART_NAME);
    uint8_t *page = p ? malloc(NVS_IMG_PAGE_SIZE) : NULL;
    if (!page) {
        return NVS_IMG_UNKNOWN; // could not look: never encrypt or migrate what was not classified
    }
    nvs_image_tally_t t           = {0};
    bool              read_failed = false;
    for (size_t off = 0; off + NVS_IMG_PAGE_SIZE <= p->size; off += NVS_IMG_PAGE_SIZE) {
        if (esp_partition_read(p, off, page, NVS_IMG_PAGE_SIZE) == ESP_OK) {
            nvs_image_tally_page(&t, page);
        } else {
            read_failed = true;
        }
    }
    free(page);
    return read_failed ? NVS_IMG_UNKNOWN : nvs_image_tally_kind(&t);
}

// A read error may be transient: look a few times before giving up
static nvs_image_kind_t scan_partition(void) {
    nvs_image_kind_t k = NVS_IMG_UNKNOWN;
    for (int i = 0; i < 3 && k == NVS_IMG_UNKNOWN; i++) {
        k = scan_once();
    }
    return k;
}

// ---- the RAM copy --------------------------------------------------------------------------------------------------

static bool read_num(nvs_handle_t h, char const *key, uint8_t type, uint64_t *out) {
    union { uint64_t u64; int64_t i64; uint32_t u32; int32_t i32; uint16_t u16; int16_t i16; uint8_t u8; int8_t i8; } v = {0}; // widest first: {0} clears it all
    esp_err_t err;
    switch (type) {
    case NVS_TYPE_U8: err = nvs_get_u8(h, key, &v.u8); *out = v.u8; break;
    case NVS_TYPE_I8: err = nvs_get_i8(h, key, &v.i8); *out = (uint64_t)(int64_t)v.i8; break;
    case NVS_TYPE_U16: err = nvs_get_u16(h, key, &v.u16); *out = v.u16; break;
    case NVS_TYPE_I16: err = nvs_get_i16(h, key, &v.i16); *out = (uint64_t)(int64_t)v.i16; break;
    case NVS_TYPE_U32: err = nvs_get_u32(h, key, &v.u32); *out = v.u32; break;
    case NVS_TYPE_I32: err = nvs_get_i32(h, key, &v.i32); *out = (uint64_t)(int64_t)v.i32; break;
    case NVS_TYPE_U64: err = nvs_get_u64(h, key, &v.u64); *out = v.u64; break;
    case NVS_TYPE_I64: err = nvs_get_i64(h, key, &v.i64); *out = (uint64_t)v.i64; break;
    default: return false;
    }
    return err == ESP_OK;
}

// One entry's value into `s` (a new entry at its end). Strings and blobs go through a temporary that is zeroed.
static bool snap_read_one(nvs_snap_t *s, char const *ns, char const *key, uint8_t type) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = false;
    if (type == NVS_TYPE_STR || type == NVS_TYPE_BLOB) {
        size_t    len = 0;
        bool      str = type == NVS_TYPE_STR;
        esp_err_t err = str ? nvs_get_str(h, key, NULL, &len) : nvs_get_blob(h, key, NULL, &len);
        uint8_t  *buf = err == ESP_OK ? malloc(len ? len : 1) : NULL;
        if (buf) {
            err = str ? nvs_get_str(h, key, (char *)buf, &len) : nvs_get_blob(h, key, buf, &len);
            ok  = err == ESP_OK && nvs_snap_add(s, ns, key, type, 0, buf, len);
            memset(buf, 0, len);
            free(buf);
        }
    } else {
        uint64_t num;
        ok = read_num(h, key, type, &num) && nvs_snap_add(s, ns, key, type, num, NULL, 0);
    }
    nvs_close(h);
    return ok;
}

// Every entry of every namespace; false if any one could not be read (the migration then does not start)
static bool snap_collect(nvs_snap_t *s) {
    nvs_iterator_t it  = NULL;
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    bool           ok  = err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
    for (; err == ESP_OK && ok; err = nvs_entry_next(&it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        ok = snap_read_one(s, info.namespace_name, info.key, (uint8_t)info.type);
        if (!ok) {
            ESP_LOGE(TAG, "%s/%s unreadable (type 0x%02x): nothing is migrated", info.namespace_name, info.key,
                     (unsigned)info.type);
        }
    }
    nvs_release_iterator(it);
    return ok;
}

static esp_err_t write_entry(nvs_handle_t h, nvs_snap_entry_t const *e) {
    switch (e->type) {
    case NVS_TYPE_U8: return nvs_set_u8(h, e->key, (uint8_t)e->num);
    case NVS_TYPE_I8: return nvs_set_i8(h, e->key, (int8_t)e->num);
    case NVS_TYPE_U16: return nvs_set_u16(h, e->key, (uint16_t)e->num);
    case NVS_TYPE_I16: return nvs_set_i16(h, e->key, (int16_t)e->num);
    case NVS_TYPE_U32: return nvs_set_u32(h, e->key, (uint32_t)e->num);
    case NVS_TYPE_I32: return nvs_set_i32(h, e->key, (int32_t)e->num);
    case NVS_TYPE_U64: return nvs_set_u64(h, e->key, e->num);
    case NVS_TYPE_I64: return nvs_set_i64(h, e->key, (int64_t)e->num);
    case NVS_TYPE_STR: return nvs_set_str(h, e->key, e->data);
    case NVS_TYPE_BLOB: return nvs_set_blob(h, e->key, e->data, e->len);
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

// Writes the copy into the (now empty) NVS and reads every entry back; returns how many came back identical
static size_t snap_write_back(nvs_snap_t const *s) {
    size_t good = 0;
    for (size_t i = 0; i < s->n; i++) {
        nvs_handle_t h;
        bool         ok = nvs_open(s->e[i].ns, NVS_READWRITE, &h) == ESP_OK;
        if (ok) {
            ok = write_entry(h, &s->e[i]) == ESP_OK && nvs_commit(h) == ESP_OK;
            nvs_close(h);
        }
        nvs_snap_t back = {0};
        if (ok && snap_read_one(&back, s->e[i].ns, s->e[i].key, s->e[i].type) &&
            nvs_snap_entry_same(&s->e[i], &back.e[0])) {
            good++;
        } else {
            ESP_LOGE(TAG, "%s/%s not written back intact", s->e[i].ns, s->e[i].key);
        }
        nvs_snap_wipe(&back);
    }
    return good;
}

// ---- the boot paths ------------------------------------------------------------------------------------------------

static esp_err_t secure_init(nvs_sec_cfg_t const *cfg) {
    esp_err_t err = nvs_flash_secure_init((nvs_sec_cfg_t *)cfg);
    if (err == ESP_OK) {
        s_encrypted = true;
        ESP_LOGI(TAG, "NVS is encrypted (HMAC key in KEY%d)", CONFIG_LICHEN_NVS_HMAC_KEY_ID);
    }
    return err;
}

// Plaintext in flash, key in the eFuse. The copy in RAM is the only one between the erase and the write-back: a power cut
// in that window loses the settings (docs/nvs-encryption.md: back them up first). If the encrypted init fails after the
// erase, the copy goes back unencrypted so nothing is lost and the next boot tries again.
static esp_err_t migrate(nvs_sec_cfg_t const *cfg) {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }
    nvs_snap_t snap = {0};
    bool       ok   = snap_collect(&snap);
    nvs_flash_deinit();
    if (!ok) {
        nvs_snap_wipe(&snap);
        return nvs_flash_init(); // untouched, plain this boot
    }
    ESP_LOGI(TAG, "migrating %u plaintext entries into encrypted NVS", (unsigned)snap.n);
    err = __real_nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME);
    if (err == ESP_OK) {
        err = secure_init(cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "encrypted init failed (%s): the entries go back unencrypted", esp_err_to_name(err));
        err = nvs_flash_init();
        if (err != ESP_OK && __real_nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME) == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    size_t good = err == ESP_OK ? snap_write_back(&snap) : 0;
    ESP_LOGI(TAG, "%u of %u entries written back%s", (unsigned)good, (unsigned)snap.n, s_encrypted ? " encrypted" : "");
    if (good != snap.n) {
        ESP_LOGE(TAG, "migration incomplete: restore the NVS backup (docs/nvs-encryption.md)");
    }
    nvs_snap_wipe(&snap);
    return err;
}

static void register_console(void);

esp_err_t lichen_nvs_init(void) {
    register_console();
    register_slots();
    bool burned = key_burned();
    s_path      = burned ? NVS_BOOT_PATH_KEYED : NVS_BOOT_PATH_PLAIN; // every return below, failures included
    if (!burned) {
        ESP_LOGW(TAG, "no HMAC key in KEY%d: NVS stays plaintext (nothing is burned from here)",
                 CONFIG_LICHEN_NVS_HMAC_KEY_ID);
        return nvs_flash_init();
    }
    nvs_image_kind_t kind = scan_partition();
    nvs_action_t     act  = nvs_image_decide(true, burned, kind);
    if (act == NVS_ACT_REFUSE) {
        // Not nvs_flash_init(): on an encrypted partition that takes every entry for damaged and erases it. NVS stays
        // down this boot (the KEYED path: main.c never erases, nvs_boot_policy.h), the settings intact in flash.
        ESP_LOGE(TAG, "nvs partition unreadable with a key burned: NVS unavailable this boot, nothing erased");
        return ESP_ERR_INVALID_STATE;
    }
    nvs_sec_cfg_t    cfg;
    if (!derive_keys(&cfg)) {
        // Plain data stays reachable; an encrypted partition cannot be opened, and returning the error (not
        // erasing) leaves it intact for a later boot or a restore
        ESP_LOGE(TAG, "HMAC key derivation failed: %s", kind == NVS_IMG_ENCRYPTED ? "NVS unavailable" : "plain NVS");
        return kind == NVS_IMG_ENCRYPTED ? ESP_FAIL : nvs_flash_init();
    }
    esp_err_t err = act == NVS_ACT_MIGRATE ? migrate(&cfg) : secure_init(&cfg);
    memset(&cfg, 0, sizeof(cfg));
    return err;
}

// ---- fw nvs-enc ----------------------------------------------------------------------------------------------------

static char const *kind_name(nvs_image_kind_t k) {
    return k == NVS_IMG_PLAIN ? "plaintext" : k == NVS_IMG_ENCRYPTED ? "encrypted" : k == NVS_IMG_UNKNOWN ? "unreadable" : "empty";
}

#if CONFIG_LICHEN_NVS_ENCRYPT_BURN_CMD
#include "lichen_random.h"

// The one place in the image that writes an eFuse (and it is only compiled with LICHEN_NVS_ENCRYPT_BURN_CMD). Refuses
// anything but an unused block; the random key never leaves this function, esp_efuse_write_key read-protects it.
static bool key_block_all_zero(void) {
    uint8_t data[32];
    if (esp_efuse_read_block(KEY_BLK, data, 0, sizeof(data) * 8) != ESP_OK) {
        return false; // cannot tell: do not burn
    }
    uint8_t acc = 0;
    for (size_t i = 0; i < sizeof(data); i++) {
        acc |= data[i];
    }
    return acc == 0;
}

static void burn(void) {
    if (!console_from_uart()) {
        console_reply("refused: the eFuse burn runs only from the USB console, not over Wi-Fi");
        return;
    }
    // Both checks: a block with purpose unset can still hold key bits that a second write would OR into a bad key
    if (!esp_efuse_key_block_unused(KEY_BLK) || !key_block_all_zero()) {
        console_reply("KEY%d is not empty (purpose %d): nothing burned", CONFIG_LICHEN_NVS_HMAC_KEY_ID,
                      (int)esp_efuse_get_key_purpose(KEY_BLK));
        return;
    }
    uint8_t key[32];
    lichen_random_fill(key, sizeof(key));
    esp_err_t err = esp_efuse_write_key(KEY_BLK, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, key, sizeof(key));
    memset(key, 0, sizeof(key));
    ESP_LOGW(TAG, "eFuse burn of KEY%d: %s", CONFIG_LICHEN_NVS_HMAC_KEY_ID, esp_err_to_name(err));
    console_reply("burn KEY%d: %s%s", CONFIG_LICHEN_NVS_HMAC_KEY_ID, esp_err_to_name(err),
                  err == ESP_OK ? " (reboot: the NVS migrates on the next boot)" : "");
}
#endif

static void cmd_nvs_enc(char const *args) {
#if CONFIG_LICHEN_NVS_ENCRYPT_BURN_CMD
    if (strncmp(args, "burn", 4) == 0) {
        if (strcmp(args, "burn YES") == 0) {
            burn();
        } else {
            console_reply("IRREVERSIBLE: burns a random HMAC key into eFuse KEY%d. Type exactly: nvs-enc burn YES",
                          CONFIG_LICHEN_NVS_HMAC_KEY_ID);
        }
        return;
    }
#endif
    console_reply("nvs-enc: key KEY%d %s, nvs partition %s, this boot %s", CONFIG_LICHEN_NVS_HMAC_KEY_ID,
                  key_burned() ? "burned (HMAC_UP)" : "not burned", kind_name(scan_partition()),
                  s_boot == 0 ? "down (nothing erased)" : s_encrypted ? "encrypted" : "plain");
}

static void register_console(void) {
    static bool done;
    if (done) {
        return;
    }
    done = true;
#if CONFIG_LICHEN_NVS_ENCRYPT_BURN_CMD
    console_register("nvs-enc", "nvs-enc | nvs-enc burn YES (state; burn = IRREVERSIBLE eFuse key)", cmd_nvs_enc);
#else
    console_register("nvs-enc", "nvs-enc (is an HMAC key burned, is NVS plain or encrypted)", cmd_nvs_enc);
#endif
}

#endif // CONFIG_LICHEN_NVS_ENCRYPT
