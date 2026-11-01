#include "mesh_journal.h"

#include "fw_console.h"
#include "mesh.h"
#include "mesh_hist.h"
#include "sdcard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_PATH      SD_MESSAGES_DIR "/log.bin"
#define TMP_PATH      SD_MESSAGES_DIR "/log.tmp"
#define TABLE_CHATS   2048 // chats a compaction tells apart (PSRAM, about 190 KB while it runs); more keep everything
#define STAGE_BYTES   4096
#define BOOT_WAIT_MS  5000
#define FLUSH_WAIT_MS 100 // the card's batch mutex: when another batch has it, try again in RETRY_S
#define STOP_WAIT_MS  3000
#define RETRY_S       5

static char const TAG[] = "mesh_hist";

static mesh_hist_t *hist;      // PSRAM; under the mesh lock
static uint8_t     *spare;     // the buffer hist is not filling: holds out_len bytes to write while out_len > 0
static size_t       out_len;   // mesh task only
static uint32_t     out_records;
static uint32_t     retry_after_s;
static size_t       keep_n;
static void (*lock_fn)(void);
static void (*unlock_fn)(void);

// Status for "fw mesh hist": written by the mesh task (at boot by the task that starts it), read by the console.
// Aligned 32-bit values: a reading a moment old is all the console needs.
static volatile bool        on;
static char const *volatile why_off = "not started";
static volatile uint32_t    log_bytes, log_records, flushes, last_flush_s, last_flush_ms, write_errors, compactions;
static volatile bool        compact_asked;

static void turn_off(char const *why) {
    why_off = why;
    on      = false;
    ESP_LOGW(TAG, "message history off: %s (messages are kept in RAM until the next restart)", why);
}

static size_t file_read(void *ctx, uint8_t *buf, size_t cap) {
    return fread(buf, 1, cap, (FILE *)ctx);
}

static bool file_write(void *ctx, uint8_t const *buf, size_t len) {
    return fwrite(buf, 1, len, (FILE *)ctx) == len;
}

static void *psram(size_t bytes) {
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

// ---- compaction (inside an sd_begin batch) -------------------------------------------------------------------------

// Rewrites log.bin as log.tmp with the newest keep_n messages per chat (t holds the counts of log.bin), then swaps the
// files: unlink log.bin, rename. A cut between the two leaves log.tmp alone, which the next boot renames.
static char const *compact_file(mesh_hist_table_t *t, uint8_t *buf, int *err) {
    int64_t  started = esp_timer_get_time();
    uint8_t *stage   = psram(STAGE_BYTES);
    FILE    *in      = fopen(LOG_PATH, "rb");
    FILE    *out     = in ? fopen(TMP_PATH, "wb") : NULL;
    if (!stage || !in || !out) {
        *err = stage ? errno : ENOMEM;
        free(stage);
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
            unlink(TMP_PATH);
        }
        return "compaction could not start";
    }
    mesh_hist_compact_t st;
    mesh_hist_compact(file_read, in, buf, t, keep_n, file_write, out, stage, STAGE_BYTES, &st);
    bool ok = st.ok && !ferror(in);
    fclose(in);
    free(stage);
    ok    = fflush(out) == 0 && fsync(fileno(out)) == 0 && ok;
    *err  = ok ? 0 : errno;
    ok    = fclose(out) == 0 && ok;
    if (!ok) {
        unlink(TMP_PATH);
        return "compaction failed";
    }
    if (unlink(LOG_PATH) != 0 || rename(TMP_PATH, LOG_PATH) != 0) {
        *err = errno;
        return "compaction could not replace log.bin";
    }
    ESP_LOGW(TAG, "journal compacted: %lu -> %lu KB, %lu records, %lld ms", (unsigned long)(log_bytes / 1024),
             (unsigned long)(st.bytes_out / 1024), (unsigned long)st.records_out,
             (long long)((esp_timer_get_time() - started) / 1000));
    log_bytes   = (uint32_t)st.bytes_out;
    log_records = st.records_out;
    compactions++;
    return NULL;
}

static bool count_rec(mesh_hist_rec_t const *r, size_t bytes, void *ctx) {
    mesh_hist_count((mesh_hist_table_t *)ctx, r, bytes);
    return true;
}

// Boot: every record into the store (under the mesh lock, a record at a time) and counted for the compaction
static bool boot_rec(mesh_hist_rec_t const *r, size_t bytes, void *ctx) {
    lock_fn();
    mesh_hist_replay(hist, r);
    unlock_fn();
    mesh_hist_count((mesh_hist_table_t *)ctx, r, bytes);
    return true;
}

typedef struct {
    mesh_hist_table_t table;
    mesh_hist_chat_t *slots;
    uint8_t          *buf;
} work_t;

static bool work_alloc(work_t *w) {
    w->slots = psram(TABLE_CHATS * sizeof(mesh_hist_chat_t));
    w->buf   = psram(MESH_HIST_SCAN_BUF);
    if (w->slots) {
        mesh_hist_table_init(&w->table, w->slots, TABLE_CHATS);
    }
    return w->slots && w->buf;
}

static void work_free(work_t *w) {
    free(w->slots);
    free(w->buf);
}

// Reads log.bin through on_rec (pass 1), then compacts it when the rules say so. NULL when all went well.
static char const *scan_and_compact(work_t *w, mesh_hist_rec_fn on_rec, bool force, int *err) {
    struct stat st;
    if (stat(LOG_PATH, &st) != 0) {
        log_bytes = log_records = 0;
        return NULL; // no journal yet
    }
    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        *err = errno;
        return "journal not readable";
    }
    mesh_hist_scan_t scan;
    mesh_hist_scan(file_read, f, w->buf, on_rec, &w->table, &scan);
    bool read_failed = ferror(f);
    *err             = read_failed ? errno : 0;
    fclose(f);
    if (read_failed) {
        return "journal read failed"; // compacting now would drop everything after the failed read
    }
    log_bytes   = (uint32_t)st.st_size;
    log_records = scan.records;
    if (scan.torn) {
        ESP_LOGW(TAG, "journal ends in a torn record after %lu good bytes of %lu: compacting",
                 (unsigned long)scan.good_bytes, (unsigned long)st.st_size);
    }
    uint64_t live = mesh_hist_live_bytes(&w->table, keep_n);
    if (!force && !mesh_hist_should_compact((uint64_t)st.st_size, live, scan.torn)) {
        return NULL;
    }
    return compact_file(&w->table, w->buf, err);
}

// The messages directory and a leftover of a cut compaction
static char const *journal_files(int *err) {
    struct stat st;
    if (mkdir(SD_MESSAGES_DIR, 0755) != 0 && errno != EEXIST) {
        *err = errno;
        return "no messages directory";
    }
    bool have_log = stat(LOG_PATH, &st) == 0, have_tmp = stat(TMP_PATH, &st) == 0;
    if (have_tmp && !have_log && rename(TMP_PATH, LOG_PATH) != 0) {
        *err = errno; // cut after the unlink: log.tmp is the compacted journal
        return "log.tmp not renamed";
    }
    if (have_tmp && have_log) {
        unlink(TMP_PATH); // cut while writing log.tmp: log.bin is still whole
    }
    return NULL;
}

void mesh_journal_boot(size_t keep, void (*lock)(void), void (*unlock)(void)) {
    keep_n    = keep;
    lock_fn   = lock;
    unlock_fn = unlock;
    hist      = heap_caps_calloc(1, sizeof(*hist), MALLOC_CAP_SPIRAM);
    uint8_t *a = psram(MESH_HIST_PENDING);
    spare      = psram(MESH_HIST_PENDING);
    if (!hist || !a || !spare) {
        free(hist);
        free(a);
        free(spare);
        hist = NULL;
        turn_off("no memory");
        return;
    }
    mesh_hist_init(hist, a, MESH_HIST_PENDING);
    if (!sd_mounted()) {
        turn_off("no SD card");
        return;
    }
    work_t w = {0};
    if (!work_alloc(&w)) {
        work_free(&w);
        turn_off("no memory");
        return;
    }
    if (!sd_begin(BOOT_WAIT_MS)) {
        work_free(&w);
        turn_off("SD card busy");
        return;
    }
    int         err = 0;
    char const *why = journal_files(&err);
    why             = why ? why : scan_and_compact(&w, boot_rec, false, &err);
    sd_end(sd_io_from_errno(err));
    work_free(&w);
    if (why) {
        ESP_LOGW(TAG, "%s (errno %d)", why, err);
        turn_off(why);
        return;
    }
    sd_info_t info;
    sd_info(&info); // free space as sd_end left it
    if (!info.free_known || !mesh_hist_space_ok(info.free_bytes, log_bytes)) {
        turn_off("SD card full");
        return;
    }
    why_off = NULL;
    on      = true;
    ESP_LOGW(TAG, "message history: %lu messages loaded, journal %lu KB in %s", (unsigned long)hist->loaded,
             (unsigned long)(log_bytes / 1024), SD_MESSAGES_DIR);
}

// ---- the mesh task -------------------------------------------------------------------------------------------------

static bool newest(mesh_chat_key_t const *key, mesh_msg_t *m) {
    return mesh_store_chat(key, m, 1, false) == 1;
}

void mesh_journal_incoming_locked(mesh_chat_key_t const *key) {
    mesh_msg_t m;
    if (hist && on && newest(key, &m)) {
        mesh_hist_add(hist, key, &m, mesh_uptime_s());
    }
}

void mesh_journal_outgoing_locked(mesh_chat_key_t const *key) {
    mesh_msg_t m;
    if (hist && on && newest(key, &m)) {
        mesh_hist_out_new(hist, key, &m, mesh_uptime_s());
    }
}

void mesh_journal_out_state_locked(uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt, uint32_t trip_ms) {
    if (hist && on) {
        mesh_hist_out_state(hist, id, state, repeats, attempt, trip_ms, mesh_uptime_s());
    }
}

void mesh_journal_out_sweep_locked(uint32_t const *ids, size_t n) {
    if (hist && on) {
        mesh_hist_out_sweep(hist, ids, n, mesh_uptime_s());
    }
}

void mesh_journal_tick_locked(void) {
    if (!hist || !on || out_len) {
        return; // the last swapped-out buffer is still waiting for the card
    }
    uint32_t now = mesh_uptime_s();
    if (mesh_hist_should_flush(now - hist->oldest_s, hist->len, compact_asked)) {
        spare = mesh_hist_swap(hist, spare, &out_len, &out_records);
    }
}

// Appends spare[0..out_len) to log.bin in one batch. A failed or short write may leave a torn record at the end:
// nothing more is appended after it (persistence goes off), and the next boot compacts it away.
static void write_out(uint32_t wait_ms) {
    if (!on) {
        out_len = 0; // dropped: persistence went off meanwhile
        return;
    }
    if (!sd_mounted()) {
        out_len = 0;
        turn_off("SD card removed");
        return;
    }
    if (!sd_begin(wait_ms)) {
        retry_after_s = mesh_uptime_s() + RETRY_S;
        return;
    }
    int64_t started = esp_timer_get_time();
    FILE   *f       = fopen(LOG_PATH, "ab");
    bool    ok      = f && fwrite(spare, 1, out_len, f) == out_len;
    int     err     = ok ? 0 : errno;
    if (f && fclose(f) != 0 && ok) {
        ok  = false;
        err = errno;
    }
    sd_end(ok ? SD_IO_OK : sd_io_from_errno(err));
    if (!ok) {
        write_errors++;
        ESP_LOGW(TAG, "journal write of %u bytes failed (errno %d)", (unsigned)out_len, err);
        out_len = 0;
        turn_off("journal write failed");
        return;
    }
    log_bytes += (uint32_t)out_len;
    log_records += out_records;
    flushes++;
    last_flush_s  = mesh_uptime_s();
    last_flush_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    out_len       = 0;
}

// On the mesh task, without the mesh lock: two passes over log.bin, so this blocks MeshCore for as long as reading the
// journal twice takes. Only on request or past the cap (weeks of traffic); boot does the routine ones.
static void compact_now(void) {
    work_t w = {0};
    if (!work_alloc(&w)) {
        work_free(&w);
        ESP_LOGW(TAG, "no memory to compact the journal");
        return;
    }
    if (!sd_begin(FLUSH_WAIT_MS)) {
        work_free(&w);
        compact_asked = true; // the card was busy: again in RETRY_S
        retry_after_s = mesh_uptime_s() + RETRY_S;
        return;
    }
    int         err = 0;
    char const *why = scan_and_compact(&w, count_rec, true, &err);
    sd_end(sd_io_from_errno(err));
    work_free(&w);
    if (why) {
        ESP_LOGW(TAG, "%s (errno %d)", why, err);
        turn_off(why);
    } else if (log_bytes > MESH_HIST_CAP) {
        turn_off("journal full: the history kept is over the cap"); // compacting again would not help
    }
}

void mesh_journal_flush(void) {
    if (!hist) {
        return;
    }
    if (mesh_uptime_s() < retry_after_s) {
        return;
    }
    if (out_len) {
        write_out(FLUSH_WAIT_MS);
        if (out_len) {
            return; // the card was busy
        }
    }
    if (on && (compact_asked || log_bytes > MESH_HIST_CAP)) {
        compact_asked = false;
        compact_now();
    }
}

void mesh_journal_stop_locked(void) {
    if (!hist || !on) {
        return;
    }
    mesh_hist_stop(hist, mesh_uptime_s());
    if (out_len) {
        write_out(STOP_WAIT_MS); // an older buffer first: the journal stays in order
    }
    if (!out_len && hist->len) {
        spare = mesh_hist_swap(hist, spare, &out_len, &out_records);
        write_out(STOP_WAIT_MS);
    }
    if (out_len || hist->len) {
        ESP_LOGW(TAG, "message history not flushed for the update: the last messages are lost");
    }
}

// ---- console -------------------------------------------------------------------------------------------------------

void mesh_journal_console(char const *args) {
    if (strcmp(args, "compact") == 0) {
        if (!on) {
            console_reply("history is off: %s", why_off ? why_off : "?");
        } else {
            compact_asked = true;
            console_reply("compaction asked: the mesh task flushes, then compacts (\"fw mesh hist\" shows the result)");
        }
        return;
    }
    if (*args) {
        console_reply("mesh hist | mesh hist compact");
        return;
    }
    size_t   pending = 0, held = 0;
    uint32_t pending_records = 0, oldest_s = 0, dropped = 0, loaded = 0, now = mesh_uptime_s();
    if (hist) {
        lock_fn();
        pending         = hist->len;
        pending_records = hist->pending_records;
        oldest_s        = hist->oldest_s;
        held            = mesh_hist_held_count(hist);
        dropped         = hist->dropped;
        loaded          = hist->loaded;
        unlock_fn();
    }
    console_reply("history %s%s%s | journal %s %lu bytes, %lu records | loaded at boot %lu messages",
                  on ? "on" : "off", on ? "" : ": ", on ? "" : (why_off ? why_off : "?"), LOG_PATH,
                  (unsigned long)log_bytes, (unsigned long)log_records, (unsigned long)loaded);
    char last[32] = "never";
    if (flushes) {
        snprintf(last, sizeof(last), "%lu s ago", (unsigned long)(now - last_flush_s));
    }
    console_reply("pending %u bytes, %lu records (oldest %lu s) + %u bytes swapped out | %u outgoing held | dropped "
                  "%lu",
                  (unsigned)pending, (unsigned long)pending_records, (unsigned long)(pending ? now - oldest_s : 0),
                  (unsigned)out_len, (unsigned)held, (unsigned long)dropped);
    console_reply("flushes %lu, last %s (%lu ms) | write errors %lu | compactions %lu", (unsigned long)flushes, last,
                  (unsigned long)last_flush_ms, (unsigned long)write_errors, (unsigned long)compactions);
}
