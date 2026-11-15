#include "mesh_advtab_run.h"

#include "fw_console.h"
#include "lichen_fs.h"
#include "lichen_mem.h"
#include "mesh.h"
#include "meshcore_run.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[] = "mesh_adverts";

#define SD_ROOT       "/sd/lichen"
#define SD_DIR        SD_ROOT "/meshcore"
#define FILE_NAME     "adverts.bin"
#define SIDE_NAME     "adverts.new"
#define PATH_MAX_LEN  160
#define BOOT_WAIT_MS  5000
#define WRITE_WAIT_MS 100  // the card's batch lock: when another batch has it, the next loop tries again
#define STOP_WAIT_MS  3000
#define OLD_PER_CALL  16 // old per-node files removed per loop of the mesh task: never a long walk (#471)
#define SNAPSHOT_MAX  (MESH_ADVTAB_HEAD + MESH_ADVTAB_CAP * (MESH_ADVTAB_REC + MESH_ADVTAB_DATA))

static mesh_advtab_t table;
static char          dir[PATH_MAX_LEN - 24]; // MeshCore's files on /storage
static uint8_t      *taken;                  // PSRAM: the copy mesh_advtab_run_write writes; the mesh task's
static size_t        taken_len;
static uint32_t      taken_changes, saved_changes;
static uint32_t      generation;
static uint32_t      last_save_s, last_internal_s;
static bool          internal_written; // since boot: the 6 h rule counts from the last one
static bool          old_files;        // <dir>/bl still holds per-node files
static bool          wipe_files;
static bool          force;            // the takeover's first snapshot does not wait for its interval
// Console figures: written by the mesh task, read by the console (words, a stale value is fine)
static volatile uint32_t writes, write_errors, last_write_ms, last_bytes, alloc_failures;
static volatile bool     last_on_card;

static void *psram(size_t bytes) {
    return lichen_alloc(meshcore_ctx(), bytes, LICHEN_MEM_BULK);
}

static void unalloc(void *p) {
    lichen_free(meshcore_ctx(), p);
}

static void path_in(char *out, char const *base, char const *name) {
    snprintf(out, PATH_MAX_LEN, "%s/%s", base, name);
}

// ---- boot: the newest valid snapshot, then the old per-node files --------------------------------------------------

// The file when it is a valid snapshot newer than *best: its bytes replace buf's (best_len, best set)
static void consider(char const *base, char const *name, uint8_t *buf, uint8_t *scratch, size_t *best_len,
                     uint32_t *best, bool *found) {
    char path[PATH_MAX_LEN];
    path_in(path, base, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    size_t   n = fread(scratch, 1, SNAPSHOT_MAX + 1, f);
    uint32_t gen;
    fclose(f);
    if (!mesh_advtab_valid(scratch, n, &gen)) {
        ESP_LOGW(TAG, "%s is not a whole snapshot (%u bytes): not used", path, (unsigned)n);
        return;
    }
    if (!*found || gen > *best) {
        memcpy(buf, scratch, n);
        *best_len = n;
        *best     = gen;
        *found    = true;
    }
}

static void load_snapshot(void) {
    uint8_t *buf = psram(SNAPSHOT_MAX + 1), *scratch = psram(SNAPSHOT_MAX + 1);
    size_t   len   = 0;
    bool     found = false;
    if (buf && scratch) {
        consider(dir, FILE_NAME, buf, scratch, &len, &generation, &found);
        if (lichen_fs_sd_mounted() && lichen_fs_sd_begin(meshcore_ctx(), BOOT_WAIT_MS)) {
            consider(SD_DIR, FILE_NAME, buf, scratch, &len, &generation, &found);
            consider(SD_DIR, SIDE_NAME, buf, scratch, &len, &generation, &found); // a cut between remove and rename
            lichen_fs_sd_end(meshcore_ctx(), 0);
        }
        if (found) {
            size_t n = mesh_advtab_unpack(&table, buf, len);
            ESP_LOGI(TAG, "%u adverts loaded (snapshot %lu, %u bytes)", (unsigned)n, (unsigned long)generation,
                     (unsigned)len);
        }
    } else {
        ESP_LOGE(TAG, "no memory to load the advert snapshot: the table starts empty");
    }
    unalloc(buf);
    unalloc(scratch);
    saved_changes = table.changes;
}

// The per-node files of older firmware fill in the nodes the snapshot does not have
static void load_old_files(void) {
    char path[PATH_MAX_LEN], file[PATH_MAX_LEN + 260];
    path_in(path, dir, "bl");
    DIR *d = opendir(path);
    if (!d) {
        return;
    }
    size_t         taken_over = 0;
    struct dirent *e;
    uint8_t        key[MESH_ADVTAB_KEY], data[MESH_ADVTAB_DATA];
    old_files = true;
    while ((e = readdir(d)) != NULL) {
        if (!mesh_advtab_key_from_name(e->d_name, key) || mesh_advtab_get(&table, key, sizeof(key), data)) {
            continue;
        }
        snprintf(file, sizeof(file), "%s/%s", path, e->d_name);
        FILE *f = fopen(file, "rb");
        if (f) {
            size_t n = fread(data, 1, sizeof(data), f);
            fclose(f);
            taken_over += mesh_advtab_put(&table, key, sizeof(key), data, n) ? 1 : 0;
        }
    }
    closedir(d);
    force = table.changes != saved_changes; // the files go only after a snapshot holds what they added
    ESP_LOGI(TAG, "%u adverts taken over from the per-node files", (unsigned)taken_over);
}

mesh_advtab_t *mesh_advtab_run_boot(char const *files_dir) {
    void *mem = psram(mesh_advtab_bytes(MESH_ADVTAB_CAP));
    if (!mem) {
        return NULL;
    }
    snprintf(dir, sizeof(dir), "%s", files_dir);
    mesh_advtab_init(&table, mem, MESH_ADVTAB_CAP);
    load_snapshot();
    load_old_files();
    last_save_s = mesh_uptime_s();
    return &table;
}

// ---- the snapshot --------------------------------------------------------------------------------------------------

static bool changed(void) {
    return table.changes != saved_changes;
}

bool mesh_advtab_run_due(void) {
    if (taken || !table.keys) {
        return false; // one is still waiting for the card
    }
    bool     on_card = lichen_fs_sd_mounted();
    uint32_t since   = on_card || !internal_written ? last_save_s : last_internal_s;
    return (force && changed()) || mesh_advtab_due(changed(), on_card, mesh_uptime_s(), since);
}

void mesh_advtab_run_take_locked(void) {
    size_t need = mesh_advtab_pack_size(&table);
    taken       = psram(need);
    if (!taken) {
        alloc_failures++;
        last_save_s = mesh_uptime_s(); // not again in every loop
        return;
    }
    taken_len     = mesh_advtab_pack(&table, generation + 1, taken, need);
    taken_changes = table.changes;
}

// The snapshot as <base>/adverts.bin through a side file. LittleFS renames over the old file in one step; FAT does
// not, so there the old one is removed first and a cut in between leaves adverts.new, which boot reads as well.
static bool write_file(char const *base, bool fat) {
    char side[PATH_MAX_LEN], path[PATH_MAX_LEN];
    path_in(side, base, SIDE_NAME);
    path_in(path, base, FILE_NAME);
    FILE *f  = fopen(side, "wb");
    bool  ok = f && fwrite(taken, 1, taken_len, f) == taken_len;
    if (f && fclose(f) != 0) {
        ok = false;
    }
    if (ok && fat && unlink(path) != 0 && errno != ENOENT) {
        ok = false;
    }
    ok = ok && rename(side, path) == 0;
    if (!ok) {
        ESP_LOGE(TAG, "snapshot to %s failed (errno %d)", path, errno);
    }
    return ok;
}

// True: written to the card. False with *busy: the card's lock was not free, nothing tried.
static bool write_card(uint32_t wait_ms, bool *busy) {
    *busy = false;
    if (!lichen_fs_sd_mounted()) {
        return false;
    }
    if (!lichen_fs_sd_begin(meshcore_ctx(), wait_ms)) {
        *busy = true;
        return false;
    }
    bool ok = (mkdir(SD_ROOT, 0755) == 0 || errno == EEXIST) && (mkdir(SD_DIR, 0755) == 0 || errno == EEXIST) &&
              write_file(SD_DIR, true);
    lichen_fs_sd_end(meshcore_ctx(), ok ? 0 : errno);
    if (ok) {
        char path[PATH_MAX_LEN];
        path_in(path, dir, FILE_NAME);
        unlink(path); // with a card, /storage holds none
    } else {
        write_errors++;
    }
    return ok;
}

static void written(bool on_card, int64_t started) {
    saved_changes = taken_changes;
    generation++;
    force         = false;
    last_save_s   = mesh_uptime_s();
    last_on_card  = on_card;
    last_bytes    = (uint32_t)taken_len;
    last_write_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    writes++;
    if (!on_card) {
        last_internal_s  = last_save_s;
        internal_written = true;
    }
}

// Whether /storage may take a snapshot now: at a stop or the takeover always, else at most every 6 hours
static bool internal_allowed(bool always) {
    return always || !internal_written || mesh_uptime_s() - last_internal_s >= MESH_ADVTAB_INTERNAL_S;
}

static void write_taken(uint32_t wait_ms, bool always) {
    int64_t started = esp_timer_get_time();
    bool    busy;
    mesh_advtab_seal(taken, taken_len);
    if (write_card(wait_ms, &busy)) {
        written(true, started);
    } else if (busy && !always) {
        return; // kept: the next loop tries the card again
    } else if (internal_allowed(always || force)) {
        if (write_file(dir, false)) {
            written(false, started);
        } else {
            write_errors++;
        }
    }
    last_save_s = mesh_uptime_s(); // a failure waits an interval too
    unalloc(taken);
    taken = NULL;
}

static void remove_snapshots(void) {
    char path[PATH_MAX_LEN];
    path_in(path, dir, FILE_NAME);
    unlink(path);
    if (lichen_fs_sd_mounted() && lichen_fs_sd_begin(meshcore_ctx(), STOP_WAIT_MS)) {
        unlink(SD_DIR "/" FILE_NAME);
        unlink(SD_DIR "/" SIDE_NAME);
        lichen_fs_sd_end(meshcore_ctx(), 0);
    }
    wipe_files = false;
}

// A few of the old per-node files per call; the folder itself when it is empty
static void remove_old_files(void) {
    char path[PATH_MAX_LEN], file[PATH_MAX_LEN + 260];
    path_in(path, dir, "bl");
    DIR *d = opendir(path);
    if (!d) {
        old_files = false;
        return;
    }
    int            removed = 0;
    struct dirent *e;
    while (removed < OLD_PER_CALL && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            snprintf(file, sizeof(file), "%s/%s", path, e->d_name);
            unlink(file);
            removed++;
        }
    }
    closedir(d);
    if (removed < OLD_PER_CALL) {
        rmdir(path);
        old_files = false;
        ESP_LOGI(TAG, "the per-node advert files are gone");
    }
}

void mesh_advtab_run_write(void) {
    if (wipe_files) {
        remove_snapshots();
    }
    if (taken) {
        write_taken(WRITE_WAIT_MS, false);
    }
    if (old_files && !force && !taken) {
        remove_old_files();
    }
}

void mesh_advtab_run_save_locked(void) {
    if (!table.keys) {
        return;
    }
    if (taken) { // one was waiting for the card: the table is newer
        unalloc(taken);
        taken = NULL;
    }
    if (changed()) {
        mesh_advtab_run_take_locked();
        if (taken) {
            write_taken(STOP_WAIT_MS, true);
        }
    }
}

void mesh_advtab_run_wiped(void) {
    if (table.keys) {
        mesh_advtab_clear(&table);
        saved_changes = table.changes;
        wipe_files    = true;
        force         = false;
    }
}

void mesh_advtab_run_console(void) {
    console_reply("adverts: %u of %u in PSRAM, %lu refused | snapshot %lu, last %s %lu bytes in %lu ms | %lu written, "
                  "%lu failed, %lu without memory%s",
                  (unsigned)table.count, (unsigned)table.cap, (unsigned long)table.refused, (unsigned long)generation,
                  writes ? (last_on_card ? "on the card" : "on /storage") : "none yet", (unsigned long)last_bytes,
                  (unsigned long)last_write_ms, (unsigned long)writes, (unsigned long)write_errors,
                  (unsigned long)alloc_failures, old_files ? " | old per-node files still there" : "");
}
