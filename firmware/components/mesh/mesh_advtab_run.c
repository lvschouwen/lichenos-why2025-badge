#include "mesh_advtab_run.h"

#include "fw_console.h"
#include "lichen_fs.h"
#include "lichen_mem.h"
#include "mesh.h"
#include "meshcore_run.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
#define RETRY_S       5 // after a busy card lock, as the journal waits
#define OLD_PER_CALL  16 // old per-node files removed per loop of the mesh task: never a long walk (#471)
#define SLICE_US      20000 // one stretch of the takeover walk before it blocks a tick (#471: WALK_SLICE_US)
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
static DIR          *old_dir;          // open while the takeover walk is under way: its cursor
static mesh_advtab_walk_t walk;
static int64_t       slice_start_us; // when this loop's stretch of the walk began
static bool          wipe_files;
static uint32_t      retry_at_s;       // a busy card: no new try before this
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

// The file's bytes (PSRAM, the caller frees) when it is a valid snapshot; its generation in *gen
static uint8_t *read_snapshot(char const *base, char const *name, size_t *len, uint32_t *gen) {
    char        path[PATH_MAX_LEN];
    struct stat st;
    path_in(path, base, name);
    if (stat(path, &st) != 0 || st.st_size < MESH_ADVTAB_HEAD || st.st_size > SNAPSHOT_MAX) {
        return NULL;
    }
    uint8_t *buf = psram((size_t)st.st_size);
    FILE    *f   = buf ? fopen(path, "rb") : NULL;
    size_t   n   = f ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    if (f) {
        fclose(f);
    }
    if (!buf || n != (size_t)st.st_size || !mesh_advtab_valid(buf, n, gen)) {
        ESP_LOGW(TAG, "%s is not a whole snapshot (%u bytes): not used", path, (unsigned)n);
        unalloc(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

// Keeps the newer of the best so far and the file
static void consider(char const *base, char const *name, uint8_t **best, size_t *best_len, uint32_t *best_gen) {
    size_t   len = 0;
    uint32_t gen = 0;
    uint8_t *buf = read_snapshot(base, name, &len, &gen);
    if (buf && (!*best || gen > *best_gen)) {
        unalloc(*best);
        *best     = buf;
        *best_len = len;
        *best_gen = gen;
    } else {
        unalloc(buf);
    }
}

static void load_snapshot(void) {
    uint8_t *best = NULL;
    size_t   len  = 0;
    consider(dir, FILE_NAME, &best, &len, &generation);
    if (lichen_fs_sd_mounted() && lichen_fs_sd_begin(meshcore_ctx(), BOOT_WAIT_MS)) {
        consider(SD_DIR, FILE_NAME, &best, &len, &generation);
        consider(SD_DIR, SIDE_NAME, &best, &len, &generation); // a cut between remove and rename
        lichen_fs_sd_end(meshcore_ctx(), 0);
    }
    if (best) {
        size_t n = mesh_advtab_unpack(&table, best, len);
        ESP_LOGI(TAG, "%u adverts loaded (snapshot %lu, %u bytes)", (unsigned)n, (unsigned long)generation,
                 (unsigned)len);
    }
    unalloc(best);
    saved_changes = table.changes;
}

// The per-node files of older firmware fill in the nodes the snapshot does not have. Boot only opens the folder; the
// walk runs a slice per loop of the mesh task (walk_step_old), the folder staying open between loops as the cursor.
static void open_old_files(void) {
    char path[PATH_MAX_LEN];
    path_in(path, dir, "bl");
    old_dir = opendir(path);
    if (old_dir) {
        old_files = true;
        memset(&walk, 0, sizeof(walk));
    }
}

static char const *old_next(void *ctx) {
    (void)ctx;
    struct dirent *e = readdir(old_dir);
    if (esp_timer_get_time() - slice_start_us >= SLICE_US) { // as #471's walk_pace, which an app may not include
        vTaskDelay(1);                                       // a block, so IDLE on this core gets its turn
        slice_start_us = esp_timer_get_time();
    }
    return e ? e->d_name : NULL;
}

static size_t old_read(void *ctx, char const *name, uint8_t *data) {
    (void)ctx;
    char file[PATH_MAX_LEN + 260];
    snprintf(file, sizeof(file), "%s/bl/%s", dir, name);
    FILE  *f = fopen(file, "rb");
    size_t n = f ? fread(data, 1, MESH_ADVTAB_DATA, f) : 0;
    if (f) {
        fclose(f);
    }
    return n;
}

static void old_lock(void *ctx) {
    (void)ctx;
    mesh_advtab_run_lock();
}

static void old_unlock(void *ctx) {
    (void)ctx;
    mesh_advtab_run_unlock();
}

static mesh_advtab_walk_ops_t const old_ops = {old_next, old_read, old_lock, old_unlock};

static void close_old_dir(void) {
    if (old_dir) {
        closedir(old_dir);
        old_dir = NULL;
    }
}

// One slice of the takeover. At its end the files go only after a snapshot holds what they added (force asks for it).
static void walk_step_old(void) {
    slice_start_us = esp_timer_get_time();
    bool done = mesh_advtab_walk_step(&table, &walk, &old_ops, NULL, OLD_PER_CALL);
    if (!done) {
        return;
    }
    close_old_dir();
    force = table.changes != saved_changes;
    ESP_LOGI(TAG, "%u adverts taken over from the per-node files", (unsigned)walk.taken_over);
}

mesh_advtab_t *mesh_advtab_run_boot(char const *files_dir) {
    void *mem = psram(mesh_advtab_bytes(MESH_ADVTAB_CAP));
    if (!mem) {
        return NULL;
    }
    snprintf(dir, sizeof(dir), "%s", files_dir);
    mesh_advtab_init(&table, mem, MESH_ADVTAB_CAP);
    load_snapshot();
    open_old_files();
    last_save_s = mesh_uptime_s();
    return &table;
}

// ---- the snapshot --------------------------------------------------------------------------------------------------

static bool changed(void) {
    return table.changes != saved_changes;
}

static mesh_advtab_clock_t clock_now(void) {
    return (mesh_advtab_clock_t){
        .changed          = changed(),
        .on_card          = lichen_fs_sd_mounted(),
        .forced           = force,
        .internal_written = internal_written,
        .now_s            = mesh_uptime_s(),
        .last_try_s       = last_save_s,
        .last_internal_s  = last_internal_s,
    };
}

bool mesh_advtab_run_due(void) {
    if (taken || !table.keys) {
        return false; // one is still waiting for the card
    }
    mesh_advtab_clock_t c = clock_now();
    return mesh_advtab_due(&c);
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

// The snapshot as <base>/adverts.bin through a side file, which goes again when anything fails (a partial one would
// eat the drive's last blocks). LittleFS renames over the old file in one step. FAT does not: there the old one is
// removed first, and a cut in between leaves adverts.new, which boot reads as well; the next write first gives it its
// name, so it is never truncated while it is the only copy. *err: the errno of the step that failed.
static bool write_file(char const *base, bool fat, int *err) {
    char side[PATH_MAX_LEN], path[PATH_MAX_LEN];
    path_in(side, base, SIDE_NAME);
    path_in(path, base, FILE_NAME);
    if (fat && access(path, F_OK) != 0) {
        rename(side, path); // nothing to rename is the usual case
    }
    FILE *f  = fopen(side, "wb");
    bool  ok = f && fwrite(taken, 1, taken_len, f) == taken_len;
    *err     = ok ? 0 : errno;
    if (f && fclose(f) != 0 && ok) {
        ok   = false;
        *err = errno;
    }
    if (ok && fat && unlink(path) != 0 && errno != ENOENT) {
        ok   = false;
        *err = errno;
    }
    if (ok && rename(side, path) != 0) {
        ok   = false;
        *err = errno;
    }
    if (!ok) {
        unlink(side);
        ESP_LOGE(TAG, "snapshot to %s failed (errno %d)", path, *err);
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
    int  err = 0;
    bool ok  = (mkdir(SD_ROOT, 0755) == 0 || errno == EEXIST) && (mkdir(SD_DIR, 0755) == 0 || errno == EEXIST);
    if (!ok) {
        err = errno;
    }
    ok = ok && write_file(SD_DIR, true, &err);
    lichen_fs_sd_end(meshcore_ctx(), err);
    if (ok) {
        char path[PATH_MAX_LEN];
        path_in(path, dir, FILE_NAME);
        unlink(path); // with a card, /storage holds none
    }
    return ok;
}

static void written(bool on_card, int64_t started) {
    saved_changes = taken_changes;
    generation++;
    force         = false;
    last_on_card  = on_card;
    last_bytes    = (uint32_t)taken_len;
    last_write_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    writes++;
    if (!on_card) {
        last_internal_s  = mesh_uptime_s();
        internal_written = true;
    }
}

// The copy goes to the card, else to /storage when that may take one; then it is freed, written or not: a failure
// waits its interval like a success (mesh_advtab_due). Only a busy card keeps it, for a try RETRY_S later.
static void write_taken(uint32_t wait_ms, bool at_stop) {
    int64_t             started = esp_timer_get_time();
    mesh_advtab_clock_t c       = clock_now();
    bool                busy, ok;
    int                 err = 0;
    mesh_advtab_seal(taken, taken_len);
    ok = write_card(wait_ms, &busy);
    if (!ok && busy && !at_stop) {
        retry_at_s = c.now_s + RETRY_S;
        return;
    }
    bool on_card = ok, tried = ok || c.on_card;
    if (!ok && mesh_advtab_internal_ok(&c, at_stop)) {
        ok    = write_file(dir, false, &err);
        tried = true;
    }
    if (ok) {
        written(on_card, started);
    } else if (tried) {
        write_errors++;
    }
    last_save_s = mesh_uptime_s();
    unalloc(taken);
    taken = NULL;
}

// After a factory reset. A busy card keeps the request for RETRY_S later; at a stop there is no later (the restart
// follows), so after STOP_WAIT_MS the card's snapshot stays: an accepted gap, logged.
static void remove_snapshots(bool at_stop) {
    char path[PATH_MAX_LEN];
    path_in(path, dir, FILE_NAME);
    unlink(path);
    if (!lichen_fs_sd_mounted()) {
        wipe_files = false;
        return;
    }
    if (!lichen_fs_sd_begin(meshcore_ctx(), at_stop ? STOP_WAIT_MS : WRITE_WAIT_MS)) {
        retry_at_s = mesh_uptime_s() + RETRY_S;
        if (at_stop) {
            ESP_LOGE(TAG, "the card was busy: its advert snapshot was not removed");
        }
        return;
    }
    unlink(SD_DIR "/" FILE_NAME);
    unlink(SD_DIR "/" SIDE_NAME);
    lichen_fs_sd_end(meshcore_ctx(), 0);
    wipe_files = false;
}

// A few of the old per-node files per call; the folder itself once a call finds nothing left to remove
static void remove_old_files(void) {
    char path[PATH_MAX_LEN], file[PATH_MAX_LEN + 260];
    path_in(path, dir, "bl");
    DIR *d = opendir(path);
    if (!d) {
        old_files = false;
        return;
    }
    char           names[OLD_PER_CALL][2 * MESH_ADVTAB_KEY + 1];
    int            n = 0, others = 0;
    struct dirent *e;
    while (n < OLD_PER_CALL && (e = readdir(d)) != NULL) { // names first: no unlink while the walk is open
        if (strlen(e->d_name) == 2 * MESH_ADVTAB_KEY) {
            memcpy(names[n++], e->d_name, 2 * MESH_ADVTAB_KEY + 1);
        } else if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            others++;
        }
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        snprintf(file, sizeof(file), "%s/%.16s", path, names[i]);
        unlink(file);
    }
    if (n == 0) {
        bool gone = others == 0 && rmdir(path) == 0;
        old_files = false; // a folder that holds something else stays; nothing more is tried
        ESP_LOGI(TAG, "the per-node advert files are gone%s", gone ? "" : " (the folder holds other files and stays)");
    }
}

void mesh_advtab_run_write(void) {
    if (retry_at_s && mesh_uptime_s() < retry_at_s) {
        return; // a busy card: not in every loop
    }
    retry_at_s = 0;
    if (wipe_files) {
        remove_snapshots(false);
    }
    if (taken) {
        write_taken(WRITE_WAIT_MS, false);
    }
    if (old_dir) {
        walk_step_old(); // the removal below starts only when the walk has ended and a snapshot holds its result
    } else if (old_files && !force && !taken) {
        remove_old_files();
    }
}

void mesh_advtab_run_save_locked(void) {
    if (!table.keys) {
        return;
    }
    if (wipe_files) { // a factory reset restarts right after its erase: the snapshots go now, also the card's
        remove_snapshots(true);
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
        close_old_dir(); // a takeover under way would put the old nodes back; the files are removed
        unalloc(taken); // a copy taken before the erase is not written after it
        taken         = NULL;
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
