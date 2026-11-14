#include "settings_backup_io.h"

#include "settings_backup_swap.h"

#include "esp_app_desc.h"
#include "lichen_mem.h"
#include "lichen_sys_app.h"
#include "esp_log.h"
#include "nvs.h"
#include "settings_fail.h"
#include "sdcard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char const TAG[] = "backup";

#define FILE_CAP  8192 // 18 short lines and a header are about 1 KB; anything larger is not ours
#define VALUES_MAX SB_KEYS_MAX // read_all writes sb_key_count() values, which the table caps at SB_KEYS_MAX

// The changes of the last plan; one load at a time (the screen guards it). A PSRAM grant that lives from plan to apply
// only: freed by apply or by the next plan, so the 2.3 KB is not held for good in internal .bss
static sb_value_t *s_held;
static size_t      s_held_count;

static void held_release(void) {
    lichen_free(lichen_sys_ctx_os(), s_held);
    s_held       = NULL;
    s_held_count = 0;
}

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

// The three names of the backup (settings_backup_swap.h)
static char const *path_of(sbs_name_t n) {
    static char const *const paths[SBS_COUNT] = {SETTINGS_BACKUP_PATH, SD_LICHEN_DIR "/settings.new", SD_LICHEN_DIR "/settings.old"};
    return paths[n];
}

static bool fs_write(void *ctx, sbs_name_t n, char const *buf, size_t len) {
    (void)ctx;
    FILE *f = fopen(path_of(n), "w");
    if (!f) {
        return false;
    }
    bool ok = fwrite(buf, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

static bool fs_rename(void *ctx, sbs_name_t from, sbs_name_t to) {
    (void)ctx;
    return rename(path_of(from), path_of(to)) == 0;
}

static bool fs_remove(void *ctx, sbs_name_t n) {
    (void)ctx;
    return remove(path_of(n)) == 0 || errno == ENOENT;
}

static sbs_ops_t const FS_OPS = {fs_write, fs_rename, fs_remove};

// Writes buf through the swap (a readable copy at every step); false when the sequence did not complete
static bool write_file(char const *buf, size_t len) {
    bool ok = sbs_save(&FS_OPS, NULL, buf, len);
    if (!ok) {
        int err = errno; // the log call may change it, and the caller classifies the card's state by it
        ESP_LOGW(TAG, "writing the backup failed: errno %d", err);
        errno = err;
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
    save_scratch_t *s = lichen_alloc(lichen_sys_ctx_os(), sizeof(*s), LICHEN_MEM_BULK);
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
    lichen_free(lichen_sys_ctx_os(), s);
    ESP_LOGI(TAG, "save: %d", (int)status);
    return status;
}

// What a load reads: the text of the file being probed and the values it parsed to, both in PSRAM
typedef struct {
    char       *text;   // FILE_CAP + 2: read FILE_CAP + 1 bytes to spot an oversize file, plus the NUL
    sb_value_t *parsed; // VALUES_MAX
    size_t      count;  // values in parsed once a probe says SBS_VALID
    sb_result_t refused; // why the first refused file was (SB_OK: none)
    int         io_err;  // errno of a read that failed (not a missing file)
} load_t;

static void note_refused(load_t *l, sb_result_t r) {
    if (l->refused.err == SB_OK) {
        l->refused = r;
    }
}

// Reads one candidate and validates it completely: the file is from a removable card, so nothing of it is trusted
// before sb_parse accepts every line (the same rule whichever name it was found under)
static sbs_state_t probe_file(void *ctx, sbs_name_t n) {
    load_t *l = ctx;
    FILE   *f = fopen(path_of(n), "r");
    if (!f) {
        if (errno != ENOENT) { // the card failed, not "no such file": for the main file the load stops here, it does not
            l->io_err = errno; // fall back to a leftover that may be older
            return n == SBS_MAIN ? SBS_INVALID : SBS_ABSENT;
        }
        return SBS_ABSENT;
    }
    size_t len = fread(l->text, 1, FILE_CAP + 1, f);
    bool   bad = ferror(f) != 0;
    fclose(f);
    if (bad) {
        l->io_err = EIO;
        return SBS_INVALID;
    }
    if (len > FILE_CAP) { // not a settings file
        note_refused(l, (sb_result_t){SB_ERR_TOO_LONG, 1, 0});
        return SBS_INVALID;
    }
    l->text[len] = '\0';
    if (n == SBS_NEW && !sb_complete(l->text, len)) { // a save cut mid-write can still parse as a smaller file
        return SBS_INVALID;
    }
    sb_result_t r = sb_parse(l->text, len, l->parsed, VALUES_MAX, &l->count);
    if (r.err != SB_OK) {
        note_refused(l, r);
        return SBS_INVALID;
    }
    l->refused = r; // the result of the copy in use: its ignored count
    return SBS_VALID;
}

// Picks and reads the backup (MAIN, else a copy a cut save left); on SBK_OK l->parsed holds the values
static sbk_status_t load_backup(load_t *l, sb_result_t *result) {
    if (!sd_mounted()) {
        return SBK_NO_CARD;
    }
    if (!sd_begin(2000)) {
        return SBK_IO;
    }
    bool       saw;
    sbs_name_t found = sbs_choose(probe_file, l, &saw);
    sd_end(l->io_err && found == SBS_COUNT ? sd_io_from_errno(l->io_err) : SD_IO_OK);
    *result = l->refused;
    if (found != SBS_COUNT) {
        return SBK_OK;
    }
    return l->io_err ? SBK_IO : saw ? SBK_INVALID : SBK_NO_FILE;
}

sbk_status_t settings_backup_plan(settings_backup_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));
    held_release();
    if (!sd_mounted()) { // before the ~8 KB below, so no card is never reported as no memory
        return SBK_NO_CARD;
    }
    // 2.3 KB of values, off the caller's stack (#209)
    load_t l = {0};
    l.text   = lichen_alloc(lichen_sys_ctx_os(), FILE_CAP + 2, LICHEN_MEM_BULK);
    l.parsed = lichen_alloc(lichen_sys_ctx_os(), VALUES_MAX * sizeof(*l.parsed), LICHEN_MEM_BULK);
    s_held   = lichen_alloc(lichen_sys_ctx_os(), VALUES_MAX * sizeof(*s_held), LICHEN_MEM_BULK);
    if (!l.text || !l.parsed || !s_held) {
        lichen_free(lichen_sys_ctx_os(), l.text);
        lichen_free(lichen_sys_ctx_os(), l.parsed);
        held_release();
        return SBK_MEMORY;
    }
    sbk_status_t status = load_backup(&l, &plan->result);
    lichen_free(lichen_sys_ctx_os(), l.text);
    if (status != SBK_OK) {
        lichen_free(lichen_sys_ctx_os(), l.parsed);
        held_release();
        return status;
    }
    size_t       n      = l.count;
    sb_value_t  *parsed = l.parsed;
    for (size_t i = 0; i < n; i++) {
        sb_value_t now;
        current_value(parsed[i].key, &now);
        if (!sb_value_equal(&now, &parsed[i])) {
            s_held[s_held_count++] = parsed[i];
        }
    }
    lichen_free(lichen_sys_ctx_os(), parsed);
    plan->changed = s_held_count;
    sb_summary(s_held, s_held_count, plan->result.ignored, plan->summary, sizeof(plan->summary));
    sbk_status_t st = s_held_count ? SBK_OK : SBK_NOTHING;
    if (!s_held_count) {
        held_release(); // nothing to apply: nothing to hold
    }
    return st;
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
        settings_fail_note(v->key->key, err);
        ESP_LOGE(TAG, "storing %s.%s failed: %s", v->key->ns, v->key->key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

void settings_backup_discard(void) {
    held_release();
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
    held_release();
    return all ? SBK_OK : SBK_IO;
}
