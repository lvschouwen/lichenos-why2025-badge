// Factory reset (#66): the erasing, at boot. NVS goes key by key (factory_plan.c decides), never the whole partition:
// the badge's own record (crashes, PSRAM tuning, the C6's state) stays, and so does any namespace Lichen does not own.
// otadata, the app slots, phy_init, coredump and assets are other partitions and are never opened here. /storage is
// mounted and emptied file by file except the OS's files (the C6 image, lora.toml); only a drive that would not mount
// is formatted. The SD card is not touched.
#include "factory_reset.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "fw_console.h"
#include "nvs.h"
#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[] = "factory";

#define ERASE_BATCH 32 // NVS entries or file names collected per pass (no erasing while an iterator is open)
#define MAX_PASSES  64
#define PATH_MAX_   256
#define TREE_DEPTH  8

typedef struct {
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
} nvs_name_t;

bool factory_reset_request(factory_level_t level) {
    nvs_handle_t nvs;
    if (level == FACTORY_NONE || nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u8(nvs, FACTORY_PENDING_KEY, (uint8_t)level) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    ESP_LOGW(TAG, "factory reset (%s) %s for the next boot", factory_level_name(level), ok ? "saved" : "NOT saved");
    return ok;
}

static uint8_t pending_get(void) {
    nvs_handle_t nvs;
    uint8_t      v = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, FACTORY_PENDING_KEY, &v);
        nvs_close(nvs);
    }
    return v;
}

static void pending_set(uint8_t v) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (v) {
        nvs_set_u8(nvs, FACTORY_PENDING_KEY, v);
    } else {
        nvs_erase_key(nvs, FACTORY_PENDING_KEY);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Up to cap entries this level erases (or, with print, lists every entry and its fate); returns how many were found
static size_t nvs_collect(factory_level_t level, nvs_name_t *out, size_t cap, bool print) {
    nvs_iterator_t it = NULL;
    size_t         n  = 0;
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

static void nvs_wipe(factory_level_t level) {
    nvs_name_t *batch  = malloc(ERASE_BATCH * sizeof(*batch));
    unsigned    erased = 0, failed = 0;
    if (!batch) {
        ESP_LOGE(TAG, "no memory: NVS not reset");
        return;
    }
    for (int pass = 0; pass < MAX_PASSES; pass++) {
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
            if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
                erased++;
            } else {
                failed++;
                ESP_LOGE(TAG, "%s/%s not erased: %s", batch[i].ns, batch[i].key, esp_err_to_name(err));
            }
        }
        if (failed) {
            break; // the same keys would come back every pass
        }
    }
    free(batch);
    ESP_LOGW(TAG, "NVS reset (%s): %u entries erased%s", factory_level_name(level), erased,
             failed ? ", some failed" : "");
}

void factory_reset_boot_nvs(void) {
    factory_boot_t plan = factory_boot_plan(pending_get());
    if (plan.nvs) {
        ESP_LOGW(TAG, "factory reset (%s): erasing NVS", factory_level_name(plan.level));
        nvs_wipe(plan.level);
        pending_set(plan.after_nvs);
        system_restart_note(plan.level == FACTORY_ALL ? "factory reset: everything" : "factory reset: settings");
    } else if (!plan.storage && pending_get()) {
        pending_set(0); // a value this firmware does not know
    }
}

// ---- /storage -------------------------------------------------------------------------------------------------------

static bool is_dir(char const *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Removes everything inside dir (top: except what the plan keeps), then dir itself unless top. Counts what failed.
static void rm_tree(char *path, size_t len, int depth, bool top, unsigned *failed) {
    char(*names)[256] = malloc(ERASE_BATCH * sizeof(*names));
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
        while (n < ERASE_BATCH && (e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || (top && factory_storage_keeps(e->d_name))) {
                continue;
            }
            strlcpy(names[n++], e->d_name, sizeof(names[0]));
        }
        closedir(d);
        unsigned before = *failed;
        for (size_t i = 0; i < n; i++) {
            int w = snprintf(path + len, PATH_MAX_ - len, "/%s", names[i]);
            if (w <= 0 || (size_t)w >= PATH_MAX_ - len) {
                (*failed)++;
                continue;
            }
            if (is_dir(path)) {
                if (depth < TREE_DEPTH) {
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

bool factory_reset_boot_storage(bool storage_ok) {
    uint8_t        pending = pending_get();
    factory_boot_t plan    = factory_boot_plan(pending);
    if (!plan.storage) {
        return storage_ok;
    }
    if (storage_ok) {
        ESP_LOGW(TAG, "factory reset (all): emptying %s (the C6 image and lora.toml stay)", STORAGE_BASE);
        char     path[PATH_MAX_];
        unsigned failed = 0;
        strlcpy(path, STORAGE_BASE, sizeof(path));
        rm_tree(path, strlen(path), 0, true, &failed);
        if (failed) {
            ESP_LOGE(TAG, "%u files or folders on %s would not go", failed, STORAGE_BASE);
        }
    } else {
        // A drive that does not mount holds nothing readable: format it (the C6 image and lora.toml were lost with it)
        ESP_LOGW(TAG, "factory reset (all): %s did not mount, formatting it", STORAGE_BASE);
        esp_vfs_fat_mount_config_t cfg = {.format_if_mount_failed = true, .max_files = 8, .allocation_unit_size = 4096};
        esp_err_t                  err = esp_vfs_fat_spiflash_format_cfg_rw_wl(STORAGE_BASE, "storage", &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        } else {
            storage_ok = storage_mount();
        }
    }
    pending_set(0); // once only: a drive that cannot be emptied must not take each boot's new settings with it
    return storage_ok;
}

// ---- console ----------------------------------------------------------------------------------------------------------

static void plan_print(factory_level_t level) {
    console_reply("factory reset '%s', dry run (nothing is erased):", factory_level_name(level));
    nvs_collect(level, NULL, 0, true);
    if (level != FACTORY_ALL) {
        console_reply("  %s: all kept; SD card: not touched", STORAGE_BASE);
        return;
    }
    DIR *d = opendir(STORAGE_BASE);
    if (!d) {
        console_reply("  %s: not mounted (would be formatted)", STORAGE_BASE);
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
                      "and every file on the drive too)");
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
    system_restart(level == FACTORY_ALL ? "factory reset all (console)" : "factory reset settings (console)");
}

void factory_reset_console_register(void) {
    console_register("factory-reset", "factory-reset plan [settings|all] | settings YES | all YES", cmd_factory);
}
