#include "settings_backup_io.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdcard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char const TAG[] = "backup";

#define FILE_CAP  8192 // 18 short lines and a header are about 1 KB; anything larger is not ours
#define PATH_NEW  SD_LICHEN_DIR "/settings.new"
#define VALUES_MAX SB_KEYS_MAX // read_all writes sb_key_count() values, which the table caps at SB_KEYS_MAX

static sb_value_t s_held[VALUES_MAX]; // the changes of the last plan; one load at a time (the screen guards it)
static size_t     s_held_count;

static void current_value(sb_key_t const *key, sb_value_t *out) {
    memset(out, 0, sizeof(*out));
    out->key = key;
    nvs_handle_t nvs;
    bool         have = false;
    if (nvs_open(key->ns, NVS_READONLY, &nvs) == ESP_OK) {
        if (key->type == SB_U8) {
            uint8_t v;
            have = nvs_get_u8(nvs, key->key, &v) == ESP_OK;
            if (have) {
                out->num = v;
            }
        } else {
            size_t len = sizeof(out->str);
            have       = nvs_get_str(nvs, key->key, out->str, &len) == ESP_OK;
        }
        nvs_close(nvs);
    }
    sb_value_t parsed; // a stored value the table would refuse (older firmware) reads as the default
    char       text[SB_STR_MAX];
    if (have) {
        sb_value_text(out, text, sizeof(text));
        have = sb_value_parse(key, text, &parsed);
    }
    if (!have) {
        snprintf(text, sizeof(text), "%u", (unsigned)key->def);
        sb_value_parse(key, key->type == SB_U8 ? text : key->def_str, out);
    }
}

static size_t read_all(sb_value_t *out) {
    size_t n = sb_key_count();
    for (size_t i = 0; i < n; i++) {
        current_value(sb_key_at(i), &out[i]);
    }
    return n;
}

// Writes buf to PATH_NEW and swaps it in (FAT rename does not replace); false on any failure
static bool write_file(char const *buf, size_t len) {
    FILE *f = fopen(PATH_NEW, "w");
    if (!f) {
        return false;
    }
    bool ok = fwrite(buf, 1, len, f) == len;
    ok      = fclose(f) == 0 && ok;
    if (ok) {
        remove(SETTINGS_BACKUP_PATH);
        ok = rename(PATH_NEW, SETTINGS_BACKUP_PATH) == 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "writing the backup failed: errno %d", errno);
        remove(PATH_NEW);
    }
    return ok;
}

// The values and the file text of one save, in PSRAM: VALUES_MAX values on the stack would be a 2.3 KB frame (#209)
typedef struct {
    sb_value_t vals[VALUES_MAX];
    char       buf[FILE_CAP];
} save_scratch_t;

sbk_status_t settings_backup_save(void) {
    if (!sd_mounted()) {
        return SBK_NO_CARD;
    }
    save_scratch_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s) {
        return SBK_MEMORY;
    }
    size_t       n      = read_all(s->vals);
    size_t       len    = sb_format(esp_app_get_description()->version, s->vals, n, s->buf, FILE_CAP);
    sbk_status_t status = SBK_IO;
    if (len && sd_begin(2000)) {
        bool ok = write_file(s->buf, len);
        sd_end(ok ? SD_IO_OK : sd_io_from_errno(errno));
        status = ok ? SBK_OK : SBK_IO;
    } else if (!len) {
        status = SBK_MEMORY;
    }
    free(s);
    ESP_LOGI(TAG, "save: %d", (int)status);
    return status;
}

// The file's text, NUL terminated, in PSRAM; NULL with *status set
static char *read_file(size_t *len, sbk_status_t *status) {
    if (!sd_mounted()) {
        *status = SBK_NO_CARD;
        return NULL;
    }
    char *buf = heap_caps_malloc(FILE_CAP + 2, MALLOC_CAP_SPIRAM) /* read FILE_CAP + 1 bytes to spot an oversize file, plus the NUL */;
    if (!buf) {
        *status = SBK_MEMORY;
        return NULL;
    }
    *status = SBK_IO;
    if (!sd_begin(2000)) {
        free(buf);
        return NULL;
    }
    FILE *f = fopen(SETTINGS_BACKUP_PATH, "r");
    int   err = errno;
    *len      = 0;
    if (f) {
        *len = fread(buf, 1, FILE_CAP + 1, f);
        err  = ferror(f) ? EIO : 0;
        fclose(f);
        *status = err ? SBK_IO : SBK_OK;
    } else if (err == ENOENT) {
        *status = SBK_NO_FILE;
    }
    sd_end(*status == SBK_IO ? sd_io_from_errno(err) : SD_IO_OK);
    if (*status == SBK_OK && *len > FILE_CAP) { // not a settings file
        *status = SBK_INVALID;
    }
    if (*status != SBK_OK && *status != SBK_INVALID) {
        free(buf);
        return NULL;
    }
    buf[*len] = '\0';
    return buf;
}

sbk_status_t settings_backup_plan(settings_backup_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));
    s_held_count = 0;
    size_t       len;
    sbk_status_t status;
    char        *text = read_file(&len, &status);
    if (!text) {
        return status;
    }
    if (status == SBK_INVALID) { // oversize: no values to hold, so no allocation that could turn it into SBK_MEMORY
        free(text);
        plan->result = (sb_result_t){SB_ERR_TOO_LONG, 1, 0};
        return SBK_INVALID;
    }
    // 2.3 KB of values, off the caller's stack (#209)
    sb_value_t *parsed = heap_caps_malloc(VALUES_MAX * sizeof(*parsed), MALLOC_CAP_SPIRAM);
    if (!parsed) {
        free(text);
        return SBK_MEMORY;
    }
    size_t n     = 0;
    plan->result = sb_parse(text, len, parsed, VALUES_MAX, &n);
    free(text);
    if (plan->result.err != SB_OK) {
        free(parsed);
        return SBK_INVALID;
    }
    for (size_t i = 0; i < n; i++) {
        sb_value_t now;
        current_value(parsed[i].key, &now);
        if (!sb_value_equal(&now, &parsed[i])) {
            s_held[s_held_count++] = parsed[i];
        }
    }
    free(parsed);
    plan->changed = s_held_count;
    sb_summary(s_held, s_held_count, plan->result.ignored, plan->summary, sizeof(plan->summary));
    return s_held_count ? SBK_OK : SBK_NOTHING;
}

static bool store(sb_value_t const *v) {
    nvs_handle_t nvs;
    if (nvs_open(v->key->ns, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = v->key->type == SB_U8 ? nvs_set_u8(nvs, v->key->key, (uint8_t)v->num) : nvs_set_str(nvs, v->key->key, v->str);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing %s.%s failed: %s", v->key->ns, v->key->key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

sbk_status_t settings_backup_apply(size_t *applied) {
    *applied = 0;
    for (size_t i = 0; i < s_held_count; i++) {
        if (store(&s_held[i])) {
            (*applied)++;
        }
    }
    bool all = *applied == s_held_count;
    ESP_LOGI(TAG, "load: %u of %u stored", (unsigned)*applied, (unsigned)s_held_count);
    s_held_count = 0;
    return all ? SBK_OK : SBK_IO;
}
