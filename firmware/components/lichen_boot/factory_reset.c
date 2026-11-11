// Factory reset (#66): the erasing, at boot (factory_plan.c decides what goes). otadata, the app slots, phy_init,
// coredump and assets are other partitions and are never opened here; the SD card is not touched.
// Settings: NVS key by key; the badge's own record (crashes, PSRAM tuning, the C6's state) and namespaces Lichen does
// not own stay.
// Everything: erased from flash, not unlinked. The badge has no flash encryption, so a deleted file or NVS entry would
// leave the old MeshCore private key readable in the chip. NVS: the kept entries go to RAM, the nvs partition is
// erased whole, they are written back. /storage: the OS files (C6 bundle, lora.toml) go to PSRAM, the storage
// partition is erased whole and formatted, they are written back. Each step is a pending state (factory_plan.h), so a
// power cut repeats the step it hit; one that hits the drive's erase loses the PSRAM copy, and the next boot erases
// again without it (no C6 bundle: the flasher leaves the C6 alone until "badge.sh ota-c6" brings one; no lora.toml:
// the default preset).
#include "psram_speed.h"
#include "factory_reset.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_nvs.h"
#include "nvs.h"
#include "nvs_erase_os.h"
#include "nvs_fail.h"
#include "nvs_flash.h"
#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[] = "factory";

#define ERASE_BATCH  32 // NVS entries collected per pass (no erasing while an iterator is open)
#define MAX_PASSES   64
#define MAX_KEPT     64 // NVS entries carried over a full erase
#define MAX_STAGED   16 // OS files carried over the drive's erase
#define STAGED_MAX   (2u * 1024 * 1024) // one file; the C6 app is ~1.5 MB
#define ERASE_CHUNK  (64u * 1024)
#define STORAGE_PART STORAGE_LABEL
#define PATH_BUF     256

typedef struct {
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
} nvs_name_t;

bool factory_reset_request(factory_level_t level) {
    psram_speed_no_rearm(); // the restart that follows comes back at 80 MHz whatever the old record said (#121)
    nvs_handle_t nvs;
    if (level == FACTORY_NONE || nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    bool ok = fw_nvs_ok(TAG, FACTORY_WIFI_WIPE_KEY, nvs_set_u8(nvs, FACTORY_WIFI_WIPE_KEY, 1)) && // first: pending implies it
              fw_nvs_ok(TAG, FACTORY_PENDING_KEY, nvs_set_u8(nvs, FACTORY_PENDING_KEY, (uint8_t)level)) &&
              fw_nvs_ok(TAG, FACTORY_PENDING_KEY, nvs_commit(nvs));
    nvs_close(nvs);
    ESP_LOG_LEVEL(
        ok ? ESP_LOG_INFO : ESP_LOG_ERROR,
        TAG,
        "factory reset (%s) %s for the next boot",
        factory_level_name(level),
        ok ? "saved" : "NOT saved"
    );
    return ok;
}

static uint8_t state_get(char const *key) {
    nvs_handle_t nvs;
    uint8_t      v = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, key, &v);
        nvs_close(nvs);
    }
    return v;
}

static bool state_set(char const *key, uint8_t v) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = v ? nvs_set_u8(nvs, key, v) : nvs_erase_key(nvs, key);
    bool      ok  = (v ? fw_nvs_ok(TAG, key, err) : fw_nvs_erased(TAG, key, err)) && fw_nvs_ok(TAG, key, nvs_commit(nvs));
    nvs_close(nvs);
    return ok;
}

static uint8_t pending_get(void) {
    return state_get(FACTORY_PENDING_KEY);
}

static bool pending_set(uint8_t v) {
    return state_set(FACTORY_PENDING_KEY, v);
}

// This boot's news for the notification service, which starts after the erasing (factory_reset_post_notices)
static bool    notice_nvs_not_flash;  // "everything" fell back to key by key: the old entries are still in flash
static bool    notice_storage_failed; // the drive's erase failed every try
static uint8_t carry;                 // the next step, when NVS could not keep it (factory_nvs_after)
static void  (*notice_error)(char const *title, char const *body); // lichen_os: an error notice (#245)

// ---- NVS, settings: key by key ---------------------------------------------------------------------------------------

// Up to cap entries this level erases (or, with print, lists every entry and its fate); returns how many were found
static size_t nvs_collect(factory_level_t level, nvs_name_t *out, size_t cap, bool print) {
    nvs_iterator_t it  = NULL;
    size_t         n   = 0;
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && (print || n < cap)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        bool erase = factory_nvs_erases(info.namespace_name, info.key, level);
        if (print) {
            console_reply("  nvs %-5s %s/%s", erase ? "ERASE" : "keep", info.namespace_name, info.key);
        } else if (erase) {
            strlcpy(out[n].ns, info.namespace_name, sizeof(out[n].ns));
            strlcpy(out[n].key, info.key, sizeof(out[n].key));
            n++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

static void nvs_wipe_keys(factory_level_t level) {
    nvs_name_t *batch  = malloc(ERASE_BATCH * sizeof(*batch));
    unsigned    erased = 0, failed = 0;
    if (!batch) {
        ESP_LOGE(TAG, "no memory: NVS not reset");
        return;
    }
    for (int pass = 0; pass < MAX_PASSES && !failed; pass++) {
        size_t n = nvs_collect(level, batch, ERASE_BATCH, false);
        if (n == 0) {
            break;
        }
        for (size_t i = 0; i < n; i++) {
            nvs_handle_t nvs;
            esp_err_t    err = nvs_open(batch[i].ns, NVS_READWRITE, &nvs);
            if (err == ESP_OK) {
                err = nvs_erase_key(nvs, batch[i].key);
                if (err == ESP_OK) {
                    err = nvs_commit(nvs);
                }
                nvs_close(nvs);
            }
            if (fw_nvs_erased(TAG, batch[i].key, err)) {
                erased++;
            } else {
                failed++; // the same keys would come back every pass
                ESP_LOGE(TAG, "%s/%s not erased: %s", batch[i].ns, batch[i].key, esp_err_to_name(err));
            }
        }
    }
    free(batch);
    ESP_LOG_LEVEL(
        failed ? ESP_LOG_WARN : ESP_LOG_INFO,
        TAG,
        "NVS reset (%s): %u entries erased%s",
        factory_level_name(level),
        erased,
        failed ? ", some failed" : ""
    );
}

// ---- NVS, everything: the whole partition, the kept entries written back -------------------------------------------

typedef struct {
    char       ns[NVS_NS_NAME_MAX_SIZE];
    char       key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    uint64_t   num;  // integer types
    void      *data; // NVS_TYPE_STR (with its NUL) and NVS_TYPE_BLOB
    size_t     len;
} kept_t;

#define GET_NUM(fn, T)                                                                                           \
    do {                                                                                                         \
        T v = 0;                                                                                                 \
        bool ok = fn(h, k->key, &v) == ESP_OK;                                                                   \
        k->num  = (uint64_t)v;                                                                                   \
        return ok;                                                                                               \
    } while (0)

static bool kept_read(nvs_handle_t h, kept_t *k) {
    switch (k->type) {
    case NVS_TYPE_U8: GET_NUM(nvs_get_u8, uint8_t);
    case NVS_TYPE_I8: GET_NUM(nvs_get_i8, int8_t);
    case NVS_TYPE_U16: GET_NUM(nvs_get_u16, uint16_t);
    case NVS_TYPE_I16: GET_NUM(nvs_get_i16, int16_t);
    case NVS_TYPE_U32: GET_NUM(nvs_get_u32, uint32_t);
    case NVS_TYPE_I32: GET_NUM(nvs_get_i32, int32_t);
    case NVS_TYPE_U64: GET_NUM(nvs_get_u64, uint64_t);
    case NVS_TYPE_I64: GET_NUM(nvs_get_i64, int64_t);
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB: {
        bool      str = k->type == NVS_TYPE_STR;
        esp_err_t err = str ? nvs_get_str(h, k->key, NULL, &k->len) : nvs_get_blob(h, k->key, NULL, &k->len);
        if (err != ESP_OK || !(k->data = malloc(k->len ? k->len : 1))) {
            return false;
        }
        return (str ? nvs_get_str(h, k->key, k->data, &k->len) : nvs_get_blob(h, k->key, k->data, &k->len)) == ESP_OK;
    }
    default: return false;
    }
}

// Integers come back from the u64 they were widened to (signed ones sign-extended there, truncated back here)
static esp_err_t kept_write(nvs_handle_t h, kept_t const *k) {
    switch (k->type) {
    case NVS_TYPE_U8: return nvs_set_u8(h, k->key, (uint8_t)k->num);
    case NVS_TYPE_I8: return nvs_set_i8(h, k->key, (int8_t)k->num);
    case NVS_TYPE_U16: return nvs_set_u16(h, k->key, (uint16_t)k->num);
    case NVS_TYPE_I16: return nvs_set_i16(h, k->key, (int16_t)k->num);
    case NVS_TYPE_U32: return nvs_set_u32(h, k->key, (uint32_t)k->num);
    case NVS_TYPE_I32: return nvs_set_i32(h, k->key, (int32_t)k->num);
    case NVS_TYPE_U64: return nvs_set_u64(h, k->key, k->num);
    case NVS_TYPE_I64: return nvs_set_i64(h, k->key, (int64_t)k->num);
    case NVS_TYPE_STR: return nvs_set_str(h, k->key, k->data);
    case NVS_TYPE_BLOB: return nvs_set_blob(h, k->key, k->data, k->len);
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

// Every entry "everything" keeps, read into RAM; returns how many (the pending flag itself is not among them)
static size_t kept_collect(kept_t *out, size_t cap) {
    nvs_iterator_t it  = NULL;
    size_t         n   = 0;
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    for (; err == ESP_OK; err = nvs_entry_next(&it)) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        bool pending = strcmp(info.namespace_name, FW_NVS_NAMESPACE) == 0 && strcmp(info.key, FACTORY_PENDING_KEY) == 0;
        if (pending || factory_nvs_erases(info.namespace_name, info.key, FACTORY_ALL)) {
            continue;
        }
        if (n == cap) {
            ESP_LOGE(TAG, "more than %u entries to keep: %s/%s lost", (unsigned)cap, info.namespace_name, info.key);
            continue;
        }
        kept_t *k = &out[n];
        *k        = (kept_t){.type = info.type};
        strlcpy(k->ns, info.namespace_name, sizeof(k->ns));
        strlcpy(k->key, info.key, sizeof(k->key));
        nvs_handle_t h;
        bool         ok = nvs_open(k->ns, NVS_READONLY, &h) == ESP_OK;
        if (ok) {
            ok = kept_read(h, k);
            nvs_close(h);
        }
        if (ok) {
            n++;
        } else {
            free(k->data);
            ESP_LOGE(TAG, "%s/%s unreadable: not kept", k->ns, k->key);
        }
    }
    nvs_release_iterator(it);
    return n;
}

// The whole partition erased and the kept entries written back. The next step is saved before them, so a power cut
// during the write-back loses only kept entries (the badge's own record), never the drive's erase.
static void nvs_wipe_full(uint8_t after) {
    kept_t   *kept = calloc(MAX_KEPT, sizeof(*kept));
    size_t    n    = kept ? kept_collect(kept, MAX_KEPT) : 0;
    esp_err_t err  = kept ? nvs_flash_deinit() : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = __real_nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME);
    }
    esp_err_t init = lichen_nvs_init(); // also after a failed erase: the rest of the boot needs NVS
    if (init == ESP_ERR_NVS_NO_FREE_PAGES || init == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        init = __real_nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME) == ESP_OK ? lichen_nvs_init() : init;
    }
    bool erased = err == ESP_OK && init == ESP_OK;
    if (!erased) {
        ESP_LOGE(TAG, "nvs partition not erased (%s / %s): key by key instead, the old entries stay in flash",
                 esp_err_to_name(err), esp_err_to_name(init));
        nvs_wipe_keys(FACTORY_ALL);
    }
    factory_nvs_next_t next = factory_nvs_after(erased, pending_set(after));
    notice_nvs_not_flash    = next.key_fallback;
    if (next.carry_in_ram) {
        ESP_LOGE(TAG, "next step not saved: the drive is erased in this boot");
        carry = after;
    }
    unsigned back = 0;
    for (size_t i = 0; i < n; i++) {
        nvs_handle_t h;
        if (erased && nvs_open(kept[i].ns, NVS_READWRITE, &h) == ESP_OK) {
            if (fw_nvs_ok(TAG, kept[i].key, kept_write(h, &kept[i])) && fw_nvs_ok(TAG, kept[i].key, nvs_commit(h))) {
                back++;
            }
            nvs_close(h);
        }
        free(kept[i].data);
    }
    free(kept);
    if (erased) {
        ESP_LOGI(TAG, "nvs partition erased, %u of %u kept entries written back", back, (unsigned)n);
    }
}

void factory_reset_boot_nvs(void) {
    uint8_t        pending = pending_get();
    factory_boot_t plan    = factory_boot_plan(pending);
    if (plan.nvs) {
        ESP_LOGI(TAG, "factory reset (%s): erasing NVS", factory_level_name(plan.level));
        if (plan.nvs_full) {
            nvs_wipe_full(plan.after_nvs);
        } else {
            nvs_wipe_keys(plan.level);
            pending_set(plan.after_nvs);
        }
        system_restart_note(plan.level == FACTORY_ALL ? "factory reset: everything" : "factory reset: settings");
    } else if (!plan.storage && pending) {
        pending_set(0); // a value this firmware does not know
    }
}

// ---- /storage: copy the OS files out, erase the partition, format, write them back --------------------------------

typedef struct {
    char     rel[48]; // "lora.toml", "c6/firmware.bin"
    uint8_t *buf;     // PSRAM
    size_t   len;
} staged_t;

static bool stage_one(char const *rel, staged_t *s) {
    char        path[96];
    struct stat st;
    snprintf(path, sizeof(path), STORAGE_BASE "/%s", rel);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return false;
    }
    if ((size_t)st.st_size > STAGED_MAX) {
        // ota-c6 only brings the C6 bundle back; lora.toml has to be copied to /storage again
        ESP_LOGE(TAG, "%s is %ld bytes, over the %u staging limit: not kept, %s", rel, (long)st.st_size,
                 (unsigned)STAGED_MAX,
                 strcmp(rel, "lora.toml") == 0 ? "copy it back"
                                               : "re-push the C6 bundle with: tools/badge.sh ota-c6 <host>");
        return false;
    }
    FILE *f = fopen(path, "rb");
    s->buf  = f ? heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM) : NULL;
    s->len  = s->buf ? fread(s->buf, 1, (size_t)st.st_size, f) : 0;
    if (f) {
        fclose(f);
    }
    if (!s->buf || s->len != (size_t)st.st_size) {
        free(s->buf);
        s->buf = NULL;
        ESP_LOGE(TAG, "%s not copied: it is lost", rel);
        return false;
    }
    strlcpy(s->rel, rel, sizeof(s->rel));
    return true;
}

// The drive's own files, into PSRAM. A drive that does not mount has nothing readable to keep.
static size_t stage_os_files(staged_t *out) {
    size_t n = 0;
    if (!storage_attach()) {
        ESP_LOGE(TAG, "%s does not mount: nothing to keep from it", STORAGE_BASE);
        return 0;
    }
    if (stage_one("lora.toml", &out[n])) {
        n++;
    }
    DIR *d = opendir(STORAGE_BASE "/c6");
    if (d) {
        struct dirent *e;
        while (n < MAX_STAGED && (e = readdir(d)) != NULL) {
            char rel[48];
            if (snprintf(rel, sizeof(rel), "c6/%s", e->d_name) < (int)sizeof(rel) && stage_one(rel, &out[n])) {
                n++;
            }
        }
        closedir(d);
    }
    storage_detach();
    return n;
}

static bool erase_partition(void) {
    esp_partition_t const *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, STORAGE_PART);
    if (!p) {
        return false;
    }
    for (size_t off = 0; off < p->size; off += ERASE_CHUNK) {
        size_t len = p->size - off < ERASE_CHUNK ? p->size - off : ERASE_CHUNK;
        if (esp_partition_erase_range(p, off, len) != ESP_OK) {
            ESP_LOGE(TAG, "erase failed at 0x%x", (unsigned)off);
            return false;
        }
        vTaskDelay(1); // the idle task (task watchdog) gets its turn through the ~30 s
    }
    return true;
}

static bool write_one(staged_t const *s) {
    char path[96];
    snprintf(path, sizeof(path), STORAGE_BASE "/%s", s->rel);
    FILE *f  = fopen(path, "wb");
    bool  ok = f && fwrite(s->buf, 1, s->len, f) == s->len;
    if (f && fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "%s not written back", s->rel);
    }
    return ok;
}

// Images first, the bundle's .md5 files last and only when every image made it: a bundle with a missing or short
// image then has no .md5 and the C6 flasher leaves the C6 alone instead of flashing half an image
static void write_back(staged_t const *s, size_t n) {
    if (!storage_attach()) {
        ESP_LOGE(TAG, "fresh drive does not mount: the C6 bundle and lora.toml are lost");
        return;
    }
    mkdir(STORAGE_BASE "/c6", 0775);
    bool     images_ok = true;
    unsigned written   = 0;
    for (int late = 0; late < 2; late++) {
        for (size_t i = 0; i < n; i++) {
            if (factory_restore_late(s[i].rel) != (late == 1) || (late && !images_ok)) {
                continue;
            }
            bool ok = write_one(&s[i]);
            written += ok;
            if (!ok && strncmp(s[i].rel, "c6/", 3) == 0) {
                images_ok = false;
            }
        }
    }
    storage_detach();
    ESP_LOG_LEVEL(
        images_ok ? ESP_LOG_INFO : ESP_LOG_WARN,
        TAG,
        "%u of %u kept files written back%s",
        written,
        (unsigned)n,
        images_ok ? "" : " (C6 bundle incomplete: .md5 files left out)"
    );
}

static bool is_dir(char const *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Removes everything inside path (top: except what the plan keeps), then path itself unless top. Counts failures.
static void rm_tree(char *path, size_t len, int depth, bool top, unsigned *failed) {
    char(*names)[256] = malloc(32 * sizeof(*names));
    if (!names) {
        (*failed)++;
        return;
    }
    for (int pass = 0; pass < MAX_PASSES * 4; pass++) {
        DIR *d = opendir(path);
        if (!d) {
            break;
        }
        size_t         n = 0;
        struct dirent *e;
        while (n < 32 && (e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || (top && factory_storage_keeps(e->d_name))) {
                continue;
            }
            strlcpy(names[n++], e->d_name, sizeof(names[0]));
        }
        closedir(d);
        unsigned before = *failed;
        for (size_t i = 0; i < n; i++) {
            int w = snprintf(path + len, PATH_BUF - len, "/%s", names[i]);
            if (w <= 0 || (size_t)w >= PATH_BUF - len) {
                (*failed)++;
            } else if (is_dir(path)) {
                if (depth < 8) {
                    rm_tree(path, len + (size_t)w, depth + 1, false, failed);
                } else {
                    (*failed)++;
                }
            } else if (unlink(path) != 0) {
                (*failed)++;
            }
            path[len] = '\0';
        }
        if (n == 0 || *failed != before) {
            break; // empty, or stuck on something that will not go
        }
    }
    free(names);
    path[len] = '\0';
    if (!top && rmdir(path) != 0) {
        (*failed)++;
    }
}

// The partition erase failed: nothing may stay readable through the filesystem. Every file goes (MeshCore's _main.id
// and _main.id.new among them) except the OS's; a drive that does not mount is formatted and gets the PSRAM copy.
static void fallback_wipe(staged_t const *staged, size_t n) {
    if (storage_attach()) {
        char     path[PATH_BUF];
        unsigned failed = 0;
        strlcpy(path, STORAGE_BASE, sizeof(path));
        rm_tree(path, strlen(path), 0, true, &failed);
        storage_detach();
        ESP_LOGE(TAG, "fallback: files deleted (%u would not go), NOT erased from flash", failed);
        if (n) {
            write_back(staged, n); // a half-done erase may have hit the OS files: the PSRAM copy goes over them
        }
        return;
    }
    bool formatted = storage_format();
    ESP_LOGE(TAG, "fallback: drive %s, NOT erased from flash", formatted ? "formatted" : "not formatted either");
    if (formatted) {
        write_back(staged, n);
    }
}

void factory_reset_boot_storage(void) {
    uint8_t        pending = carry ? carry : pending_get();
    factory_boot_t plan    = factory_boot_plan(pending);
    if (!plan.storage) {
        return;
    }
    staged_t *staged = calloc(MAX_STAGED, sizeof(*staged));
    size_t    n      = plan.restore && staged ? stage_os_files(staged) : 0;
    if (!plan.restore) {
        ESP_LOGE(TAG, "factory reset (all): an earlier erase of %s was cut off; the C6 bundle and lora.toml are gone "
                      "(badge.sh ota-c6 brings the bundle back)", STORAGE_BASE);
    }
    // From here the PSRAM copy is the only one: a power cut means erase again, without it
    if (!pending_set(FACTORY_PENDING_ERASING)) {
        ESP_LOGW(TAG, "pending state not saved: erasing anyway");
    }
    ESP_LOGI(TAG, "factory reset (all): erasing the %s partition (%u files kept in PSRAM)", STORAGE_PART, (unsigned)n);
    bool erased = erase_partition();
    if (erased) {
        storage_format(); // logs its own failure; write_back then says the drive does not mount
        write_back(staged, n);
    }
    factory_storage_next_t next = factory_storage_after(erased, state_get(FACTORY_TRIES_KEY));
    if (next.fallback_wipe) {
        fallback_wipe(staged, n);
    }
    for (size_t i = 0; staged && i < n; i++) {
        free(staged[i].buf);
    }
    free(staged);
    pending_set(next.pending);
    state_set(FACTORY_TRIES_KEY, next.tries);
    carry                 = 0;
    notice_storage_failed = next.notice;
    if (erased) {
        ESP_LOGI(TAG, "factory reset (all): %s erased from flash", STORAGE_BASE);
    } else if (next.pending) {
        ESP_LOGE(TAG, "factory reset (all): %s not erased from flash, try %u of %u on the next boot", STORAGE_BASE,
                 (unsigned)next.tries + 1, (unsigned)FACTORY_STORAGE_TRIES);
    } else {
        ESP_LOGE(TAG, "factory reset (all): %s could not be erased from flash, giving up", STORAGE_BASE);
    }
}

void factory_reset_bind_notice(void (*error)(char const *title, char const *body)) {
    notice_error = error;
}

static void post(char const *title, char const *body) {
    if (notice_error) {
        notice_error(title, body);
    } else {
        ESP_LOGE(TAG, "%s (no notification service bound)", title); // the log keeps it at least
    }
}

void factory_reset_post_notices(void) {
    if (notice_nvs_not_flash) {
        post("Factory reset could not fully erase settings",
             "The settings were deleted one by one: their old values are still in the chip's flash.");
    }
    if (notice_storage_failed) {
        post("Factory reset could not fully erase storage",
             "The files were deleted, but the drive could not be erased: old data, the MeshCore key "
             "among it, may still be in the chip's flash. Run the reset again or reflash the badge.");
    }
}

// ---- console ----------------------------------------------------------------------------------------------------------

static void plan_print(factory_level_t level) {
    console_reply("factory reset '%s', dry run (nothing is erased):", factory_level_name(level));
    if (level == FACTORY_ALL) {
        console_reply("  nvs: whole partition erased from flash; the 'keep' entries are written back");
    }
    nvs_collect(level, NULL, 0, true);
    if (level != FACTORY_ALL) {
        console_reply("  %s: all kept; SD card: not touched", STORAGE_BASE);
        return;
    }
    console_reply("  %s: whole partition erased from flash and formatted; 'keep' files written back", STORAGE_BASE);
    DIR *d = storage_mounted() ? opendir(STORAGE_BASE) : NULL;
    if (!d) {
        console_reply("  %s: not mounted (nothing to keep; it would be erased and formatted)", STORAGE_BASE);
    } else {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            console_reply("  file %-5s %s/%s", factory_storage_keeps(e->d_name) ? "keep" : "ERASE", STORAGE_BASE,
                          e->d_name);
        }
        closedir(d);
    }
    console_reply("  SD card: not touched");
}

static void cmd_factory(char const *args) {
    char            word[16] = "";
    char            yes[8]   = "";
    factory_level_t level;
    sscanf(args, "%15s %7s", word, yes);
    if (strcmp(word, "plan") == 0) {
        char lw[16] = "";
        sscanf(args, "%*s %15s", lw);
        if (!factory_level_parse(lw, &level)) {
            level = FACTORY_ALL;
        }
        plan_print(level);
        return;
    }
    if (!factory_level_parse(word, &level)) {
        console_reply("factory-reset plan [settings|all] (dry run) | settings YES | all YES (all: MeshCore identity "
                      "and every file on the drive too, erased from flash)");
        return;
    }
    if (strcmp(yes, "YES") != 0) {
        console_reply("nothing erased: add YES (factory-reset %s YES)", word);
        return;
    }
    if (!factory_reset_request(level)) {
        console_reply("could not save the reset: nothing erased");
        return;
    }
    console_reply("factory reset '%s' at the next boot: restarting", word);
    system_restart_asked(level == FACTORY_ALL ? "factory reset all (console)" : "factory reset settings (console)");
}

void factory_reset_console_register(void) {
    console_register("factory-reset", "factory-reset plan [settings|all] | settings YES | all YES", cmd_factory);
}
