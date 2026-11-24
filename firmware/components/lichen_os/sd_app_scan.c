// The SD apps' mount scan (#523): /sd/lichen/apps/<id>/manifest for every directory, one tile per app that passes.
// Only manifests are read here: no ELF code is loaded until the tile is opened. A directory without a manifest is
// left alone (every app has its data directory there, lichen_fs_sd_dir); a bad one, a wrong API, a shadowed id or a
// missing program is refused with one notification and a log line per scan.
#include "sd_app_svc.h"

#include "app_ns.h"
#include "lichen_sys_app.h"
#include "lichen_sys_mem.h"
#include "lichen_sys_notify.h"
#include "sdcard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[] = "sdapps";

#define SD_WAIT_MS    2000
#define MANIFEST_MAX  2048 // a manifest is a handful of short lines; a bigger file is no manifest

// The names of the app directories, under one batch. False: the card is not there or the folder is not.
static bool list_dirs(char names[][LICHEN_MANIFEST_ID_MAX], size_t *n) {
    *n = 0;
    if (!sd_begin(SD_WAIT_MS)) {
        return false;
    }
    DIR *d = opendir(SD_APPS_DIR);
    if (!d) {
        sd_end(SD_IO_NEUTRAL); // no apps folder yet: nothing to show
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char ns[24];
        if (e->d_type != DT_DIR || e->d_name[0] == '.' || strlen(e->d_name) >= LICHEN_MANIFEST_ID_MAX ||
            !app_ns_name(e->d_name, ns, sizeof(ns))) {
            continue; // not an app id: not ours to read
        }
        if (*n == SD_APPS_MAX) {
            ESP_LOGW(TAG, "over %d app folders, rest skipped", SD_APPS_MAX);
            break;
        }
        strlcpy(names[(*n)++], e->d_name, LICHEN_MANIFEST_ID_MAX);
    }
    closedir(d);
    sd_end(SD_IO_OK);
    return true;
}

typedef enum { READ_OK, READ_NONE, READ_BAD } read_t;

// The manifest text of dir into buf (NUL-terminated, *len bytes): READ_NONE when there is no such file
static read_t read_manifest(char const *dir, char *buf, size_t *len) {
    char path[sizeof(SD_APPS_DIR) + LICHEN_MANIFEST_ID_MAX + 12];
    snprintf(path, sizeof(path), "%s/%s/manifest", SD_APPS_DIR, dir);
    if (!sd_begin(SD_WAIT_MS)) {
        return READ_BAD;
    }
    read_t got = READ_OK;
    int    err = 0;
    int    fd  = open(path, O_RDONLY);
    if (fd < 0) {
        err = errno;
        got = err == ENOENT ? READ_NONE : READ_BAD;
    } else {
        ssize_t n = read(fd, buf, MANIFEST_MAX + 1);
        if (n < 0 || n > MANIFEST_MAX) {
            err = n < 0 ? errno : EFBIG;
            got = READ_BAD;
            n   = 0;
        }
        close(fd);
        *len = n > 0 ? (size_t)n : 0;
    }
    sd_end(got == READ_BAD ? sd_io_from_errno(err) : SD_IO_OK);
    return got;
}

static bool entry_exists(lichen_manifest_t const *m) {
    char path[sizeof(SD_APPS_DIR) + LICHEN_MANIFEST_ID_MAX + LICHEN_MANIFEST_NAME_MAX + 4];
    snprintf(path, sizeof(path), "%s/%s/%s", SD_APPS_DIR, m->id, m->entry);
    if (!sd_begin(SD_WAIT_MS)) {
        return false;
    }
    struct stat st;
    bool        ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    sd_end(SD_IO_NEUTRAL);
    return ok;
}

// One notification and one log line for an app that is not shown (once per scan: each directory is looked at once)
static void refuse(char const *why) {
    ESP_LOGW(TAG, "refused: %s", why);
    lichen_sys_notify(LN_WARN, "SD app not shown", why, 0);
}

static sd_app_rec_t *make_rec(lichen_manifest_t const *m) {
    sd_app_rec_t *rec = heap_caps_calloc(1, sizeof(*rec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rec) {
        return NULL;
    }
    rec->m        = *m;
    rec->stop_sem = xSemaphoreCreateBinary();
    if (!rec->stop_sem) {
        heap_caps_free(rec);
        return NULL;
    }
    // Every hook of the app struct is an OS trampoline: none points into the ELF (the ELF is not even loaded yet)
    rec->app = (lichen_app_t){.name             = rec->m.name,
                              .id               = rec->m.id,
                              .open             = sd_app_tramp_open,
                              .stop_for_update  = sd_app_tramp_stop,
                              // What the manifest declares, no more and no less (owner decision 2026-11-23: shown in
                              // Settings > Apps). main()'s task is the OS's doing and needs no cap; a task the app starts
                              // itself needs BACKGROUND declared
                              .caps             = rec->m.caps,
                              .internal_ram_kib = rec->m.internal_ram_kib,
                              .psram_kib        = rec->m.psram_kib};
    return rec;
}

static void register_one(lichen_manifest_t const *m) {
    sd_app_rec_t *rec = make_rec(m);
    if (!rec) {
        refuse("out of memory");
        return;
    }
    rec->ctx = lichen_register_app(&rec->app);
    if (rec->ctx == LICHEN_CTX_NONE) { // the table is full, or the id is one the OS keeps (lichen_register_app logs which)
        char why[SD_APP_MSG_LEN];
        sd_app_scan_reason(SD_APP_SCAN_SHADOW, LICHEN_MANIFEST_OK, m, m->id, why, sizeof(why));
        vSemaphoreDelete(rec->stop_sem);
        heap_caps_free(rec);
        refuse(why);
        return;
    }
    lichen_sys_mem_quota_set(rec->ctx, m->psram_kib); // #520: no quota is made for an app that joins after boot
    sd_app_link(rec);
    ESP_LOGI(TAG, "%s: tile (api %u.%u)", m->id, (unsigned)m->api_major, (unsigned)m->api_minor);
}

// again (#553): the app is a tile already and takes the new caps and budgets; a refused manifest leaves the old ones
static void scan_one(char const *dir, char *buf, bool again) {
    size_t len = 0;
    switch (read_manifest(dir, buf, &len)) {
    case READ_NONE:
        return;
    case READ_BAD: {
        char why[SD_APP_MSG_LEN];
        sd_app_scan_reason(SD_APP_SCAN_BAD_MANIFEST, LICHEN_MANIFEST_ERR_LINE, NULL, dir, why, sizeof(why));
        refuse(why);
        return;
    }
    case READ_OK:
    default:
        break;
    }
    if (!again && sd_app_known(dir)) {
        return; // still here (an app that did not stop stays until the badge restarts): neither shown twice nor refused
    }
    lichen_manifest_t     m;
    lichen_manifest_err_t err = lichen_manifest_parse(buf, len, dir, &m, NULL);
    bool                  taken = !again && err == LICHEN_MANIFEST_OK && lichen_app_by_id(m.id) != NULL;
    bool                  entry = err == LICHEN_MANIFEST_OK && entry_exists(&m);
    sd_app_scan_t         v     = sd_app_scan_verdict(err, &m, taken, entry);
    if (v == SD_APP_SCAN_OK) {
        if (!again) {
            register_one(&m);
        } else if (sd_app_apply_manifest(dir, &m)) {
            ESP_LOGI(TAG, "%s: manifest read again", dir);
        }
        return;
    }
    char why[SD_APP_MSG_LEN];
    sd_app_scan_reason(v, err, &m, dir, why, sizeof(why));
    refuse(why);
}

void sd_app_scan(void) {
    char (*names)[LICHEN_MANIFEST_ID_MAX] = heap_caps_malloc(SD_APPS_MAX * LICHEN_MANIFEST_ID_MAX, MALLOC_CAP_SPIRAM);
    char *buf                              = heap_caps_malloc(MANIFEST_MAX + 1, MALLOC_CAP_SPIRAM);
    size_t n                               = 0;
    if (names && buf && list_dirs(names, &n)) {
        for (size_t i = 0; i < n; i++) {
            scan_one(names[i], buf, false);
        }
    }
    heap_caps_free(names);
    heap_caps_free(buf);
}

void sd_app_rescan(char const *id) {
    char *buf = heap_caps_malloc(MANIFEST_MAX + 1, MALLOC_CAP_SPIRAM);
    if (buf) {
        scan_one(id, buf, true);
    }
    heap_caps_free(buf);
}
