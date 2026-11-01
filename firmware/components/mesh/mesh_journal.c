#include "mesh_journal.h"

#include "fw_console.h"
#include "mesh.h"
#include "mesh_hist.h"
#include "mesh_hist_crypto.h"
#include "sdcard.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lichen_random.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_PATH      SD_MESSAGES_DIR "/log.bin"
#define TMP_PATH      SD_MESSAGES_DIR "/log.tmp"
#define BAD_PATH      SD_MESSAGES_DIR "/log.bad" // a damaged journal as it was, kept when much followed the damage
#define BAD1_PATH     SD_MESSAGES_DIR "/log.bad.1"
#define OLD_PATH      SD_MESSAGES_DIR "/log.old" // a journal this node cannot read (another key, an older format)
#define OLD1_PATH     SD_MESSAGES_DIR "/log.old.1" // two of each are kept; neither is ever overwritten
#define BAD_KEEP_FROM 4096 // bytes after the first bad spot that make log.bad worth keeping
#define TABLE_CHATS   1024 // chats a compaction tells apart (PSRAM, about 170 KB while it runs); more keep everything
#define STAGE_BYTES   4096
#define BOOT_WAIT_MS  5000
#define FLUSH_WAIT_MS 100 // the card's batch mutex: when another batch has it, try again in RETRY_S
#define STOP_WAIT_MS  3000
#define RETRY_S       5

static char const TAG[] = "mesh_hist";

static mesh_hist_t *hist;      // PSRAM; under the mesh lock
static mesh_hist_crypt_t wcrypt; // the journal's keys (#98): set at boot, then only read (the salt survives compaction)
static uint8_t     *spare;     // the buffer hist is not filling: holds out_len bytes to write while out_len > 0
static size_t       out_len;   // mesh task only
static uint32_t     out_records;
static uint32_t     retry_after_s;
static size_t       keep_n;
static bool         drop_warned;
static void (*lock_fn)(void);
static void (*unlock_fn)(void);

// Status for "fw mesh hist": written by the mesh task (at boot by the task that starts it), read by the console.
// Aligned 32-bit values: a reading a moment old is all the console needs.
static volatile bool        on;
static char const *volatile why_off = "not started";
static volatile uint32_t    log_bytes, log_records, flushes, last_flush_s, last_flush_ms, write_errors, compactions;
static volatile uint32_t    resyncs, auth_failures;
static volatile uint32_t    boot_replay_ms, boot_records, boot_bytes; // hardware vs portable crypto on the bench
static volatile bool        compact_asked; // the mesh task compacts before its next write (boot asks when needed)

static void turn_off(char const *why) {
    why_off = why;
    on      = false;
    mh_wipe(&wcrypt, sizeof(wcrypt)); // nothing encrypts or reads the journal any more: the keys go
    ESP_LOGW(TAG, "message history off: %s (messages are kept in RAM until the next restart)", why);
}

static uint32_t clamp32(uint64_t v) {
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
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

static void random_fill(void *buf, size_t len) {
    lichen_random_fill(buf, len);
}

// Scans a whole file through on_rec with keys c (opened by its HEAD); false (with *err) when it could not be read to
// its end
static bool scan_file(char const *path, work_t *w, mesh_hist_crypt_t *c, mesh_hist_rec_fn on_rec, void *ctx,
                      mesh_hist_scan_t *scan, int *err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err = errno;
        return false;
    }
    mesh_hist_scan(file_read, f, w->buf, c, on_rec, ctx, scan);
    bool failed = ferror(f);
    *err        = failed ? errno : 0;
    fclose(f);
    return !failed;
}

// ---- compaction (inside an sd_begin batch) -------------------------------------------------------------------------

// Rewrites src as log.tmp with the newest keep_n messages per chat (t holds src's counts), then makes it log.bin:
// unlink log.bin (when src is log.bin), rename. A cut in between leaves log.tmp alone, which the next boot renames.
static char const *compact_file(char const *src, mesh_hist_table_t *t, uint8_t *buf, int *err) {
    int64_t  started = esp_timer_get_time();
    uint8_t *stage   = psram(STAGE_BYTES);
    FILE    *in      = fopen(src, "rb");
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
    mesh_hist_crypt_t   c = wcrypt; // the root; the HEAD opens the copy
    mesh_hist_compact(file_read, in, buf, &c, t, keep_n, file_write, out, stage, STAGE_BYTES, &st);
    mh_wipe(&c, sizeof(c));
    auth_failures += st.auth_failures;
    bool ok = st.ok && !ferror(in);
    fclose(in);
    free(stage);
    ok   = fflush(out) == 0 && fsync(fileno(out)) == 0 && ok;
    *err = ok ? 0 : errno;
    ok   = fclose(out) == 0 && ok;
    if (!ok) {
        unlink(TMP_PATH);
        return "compaction failed";
    }
    bool from_log = strcmp(src, LOG_PATH) == 0;
    if ((from_log && unlink(LOG_PATH) != 0) || rename(TMP_PATH, LOG_PATH) != 0) {
        *err = errno;
        return "compaction could not replace log.bin";
    }
    ESP_LOGW(TAG, "journal compacted: %lu -> %lu KB, %lu records, %lld ms", (unsigned long)(log_bytes / 1024),
             (unsigned long)(st.bytes_out / 1024), (unsigned long)st.records_out,
             (long long)((esp_timer_get_time() - started) / 1000));
    log_bytes   = clamp32(st.bytes_out);
    log_records = st.records_out;
    compactions++;
    return NULL;
}

static bool count_rec(mesh_hist_rec_t const *r, uint8_t const *raw, size_t bytes, void *ctx) {
    (void)raw;
    mesh_hist_count((mesh_hist_table_t *)ctx, r, bytes);
    return true;
}

static bool exists(char const *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// Copies kept aside are never overwritten: the plain name, else ".1", else none (-1)
static int aside_slot(char const *base0, char const *base1) {
    return mesh_hist_aside_slot(exists(base0), exists(base1));
}

// Pass 1 over log.bin, then the rewrite. A damaged journal with more than BAD_KEEP_FROM bytes after its first bad spot
// is kept as log.bad (or log.bad.1; renamed, so it costs no copy) and compacted from there. Nothing is rewritten after
// a crypto primitive failed: records would go missing.
static char const *compact_journal(work_t *w, int *err) {
    struct stat      st;
    mesh_hist_scan_t scan;
    if (stat(LOG_PATH, &st) != 0) {
        log_bytes = log_records = 0;
        return NULL; // nothing to compact
    }
    mesh_hist_crypt_t c  = wcrypt;
    bool              ok = scan_file(LOG_PATH, w, &c, count_rec, &w->table, &scan, err);
    mh_wipe(&c, sizeof(c));
    if (!ok) {
        return "journal read failed"; // compacting now would drop everything after the failed read
    }
    if (scan.crypto_error) {
        return "journal crypto failed";
    }
    if (scan.unreadable) {
        return "journal unreadable"; // it was readable at boot: the file was replaced meanwhile
    }
    char const *bad = NULL;
    if (scan.bad_seen && (uint64_t)st.st_size - scan.first_bad > BAD_KEEP_FROM) {
        int slot = aside_slot(BAD_PATH, BAD1_PATH);
        bad      = slot == 0 ? BAD_PATH : slot == 1 ? BAD1_PATH : NULL;
        if (!bad) {
            ESP_LOGW(TAG, "journal damaged, but log.bad and log.bad.1 exist: not kept again");
        } else if (rename(LOG_PATH, bad) != 0) {
            *err = errno;
            return "log.bad not kept";
        } else {
            ESP_LOGW(TAG, "journal damaged at byte %llu of %lu: kept as %s", (unsigned long long)scan.first_bad,
                     (unsigned long)st.st_size, bad);
        }
    }
    char const *why = compact_file(bad ? bad : LOG_PATH, &w->table, w->buf, err);
    if (why && bad && !exists(LOG_PATH)) {
        rename(bad, LOG_PATH); // the damaged journal is still better than none
    }
    return why;
}

// ---- boot ----------------------------------------------------------------------------------------------------------

typedef struct {
    mesh_hist_table_t *table;
    uint32_t           max_seq;
} boot_ctx_t;

// Every record into the store (under the mesh lock, a record at a time: the screens may read it) and counted, so the
// compaction decision needs no second pass
static bool boot_rec(mesh_hist_rec_t const *r, uint8_t const *raw, size_t bytes, void *ctx) {
    (void)raw;
    boot_ctx_t *b = (boot_ctx_t *)ctx;
    lock_fn();
    mesh_hist_replay(hist, r);
    unlock_fn();
    mesh_hist_count(b->table, r, bytes);
    if (r->type == MESH_HIST_MSG && r->seq > b->max_seq) {
        b->max_seq = r->seq;
    }
    return true;
}

// The HEAD search: stops at the first HEAD that opens the keys
static bool head_rec(mesh_hist_rec_t const *r, uint8_t const *raw, size_t bytes, void *ctx) {
    (void)raw, (void)bytes, (void)ctx;
    return r->type != MESH_HIST_HEAD;
}

// A file that scans whole with this node's key: its HEAD, no damage, no auth failures
static bool scans_clean(char const *path, work_t *w, int *err) {
    mesh_hist_scan_t  scan;
    mesh_hist_crypt_t c  = wcrypt;
    bool              ok = scan_file(path, w, &c, NULL, NULL, &scan, err) && scan.head && !scan.torn &&
              !scan.resyncs && !scan.auth_failures && !scan.before_head && !scan.crypto_error;
    mh_wipe(&c, sizeof(c));
    return ok;
}

// The messages directory, and what a cut compaction left (mesh_hist_files_plan)
static char const *journal_files(work_t *w, int *err) {
    if (mkdir(SD_MESSAGES_DIR, 0755) != 0 && errno != EEXIST) {
        *err = errno;
        return "no messages directory";
    }
    bool have_tmp = exists(TMP_PATH), have_bin = exists(LOG_PATH);
    bool have_bad = exists(BAD_PATH), have_bad1 = exists(BAD1_PATH);
    bool clean    = have_tmp && scans_clean(TMP_PATH, w, err);
    switch (mesh_hist_files_plan(have_bin, have_tmp, clean, have_bad || have_bad1)) {
    case MESH_HIST_FILES_KEEP:
        return NULL;
    case MESH_HIST_FILES_DROP_TMP:
        unlink(TMP_PATH);
        return NULL;
    case MESH_HIST_FILES_BAD_BACK: {
        char const *bad = mesh_hist_newest_slot(have_bad, have_bad1) == 1 ? BAD1_PATH : BAD_PATH;
        if (rename(bad, LOG_PATH) != 0) {
            *err = errno;
            return "log.bad not taken back";
        }
        unlink(TMP_PATH);
        ESP_LOGW(TAG, "journal: a cut compaction left an unfinished log.tmp: %s taken back as log.bin", bad);
        return NULL;
    }
    case MESH_HIST_FILES_TMP_WINS:
        break;
    }
    if ((have_bin && unlink(LOG_PATH) != 0) || rename(TMP_PATH, LOG_PATH) != 0) {
        *err = errno;
        return "log.tmp not taken as log.bin";
    }
    ESP_LOGW(TAG, "journal: log.tmp of a cut compaction taken as log.bin");
    return NULL;
}

// A new, empty journal: its HEAD with a new salt (the keys the writer uses from now on). Only where there is no
// journal or an empty one: "wb" truncates.
static char const *journal_fresh(int *err) {
    uint8_t head[MESH_HIST_REC_MAX];
    size_t n  = mesh_hist_crypt_new(&wcrypt) ? mesh_hist_encode_head(&wcrypt, head, sizeof(head)) : 0;
    FILE  *f  = n ? fopen(LOG_PATH, "wb") : NULL;
    bool   ok = f && fwrite(head, 1, n, f) == n;
    *err      = ok ? 0 : errno;
    if (f && fclose(f) != 0 && ok) {
        ok   = false;
        *err = errno;
    }
    if (!ok) {
        return "new journal not written";
    }
    log_bytes   = (uint32_t)n;
    log_records = 1;
    return NULL;
}

// A journal this node cannot read is moved aside, never deleted or overwritten (another badge's card, a new identity,
// an older format, or garbage)
static char const *journal_aside(int *err) {
    char const *old = aside_slot(OLD_PATH, OLD1_PATH) == 0 ? OLD_PATH : OLD1_PATH;
    if (rename(LOG_PATH, old) != 0) {
        *err = errno;
        return "unreadable journal not moved aside";
    }
    ESP_LOGW(TAG, "journal unreadable with this node's key (another badge, a new identity, an older format or "
                  "garbage): moved to %s, starting a new one", old);
    return journal_fresh(err);
}

static void note_damage(mesh_hist_scan_t const *scan) {
    auth_failures = scan->auth_failures;
    resyncs       = scan->resyncs;
    if (scan->auth_failures) {
        ESP_LOGW(TAG, "journal: %lu record(s) failed authentication, dropped", (unsigned long)scan->auth_failures);
    }
    if (scan->torn || scan->resyncs) {
        ESP_LOGW(TAG, "journal damaged: %lu bad spot(s), %llu bytes skipped%s", (unsigned long)scan->resyncs,
                 (unsigned long long)scan->skipped_bytes, scan->torn ? ", torn end" : "");
    }
}

// Finds the HEAD (pass A, usually the first record), then replays log.bin into the store with its keys (pass B, which
// also reads records a damaged first HEAD left unkeyed); asks for a compaction when the rules say so (the mesh task
// does it, after start)
static char const *journal_load(work_t *w, int *err) {
    struct stat      st;
    mesh_hist_scan_t scan;
    bool             have = stat(LOG_PATH, &st) == 0;
    memset(&scan, 0, sizeof(scan));
    if (have && !scan_file(LOG_PATH, w, &wcrypt, head_rec, NULL, &scan, err)) {
        return "journal read failed";
    }
    switch (mesh_hist_load_plan(have, have ? (uint64_t)st.st_size : 0, &scan,
                                aside_slot(OLD_PATH, OLD1_PATH) >= 0)) {
    case MESH_HIST_LOAD_FRESH:
        return journal_fresh(err);
    case MESH_HIST_LOAD_ASIDE:
        return journal_aside(err);
    case MESH_HIST_LOAD_OFF:
        return scan.crypto_error ? "journal crypto failed" : "journal unreadable, and log.old and log.old.1 are kept";
    case MESH_HIST_LOAD_USE:
        break;
    }
    boot_ctx_t b       = {.table = &w->table};
    int64_t    started = esp_timer_get_time();
    if (!scan_file(LOG_PATH, w, &wcrypt, boot_rec, &b, &scan, err)) {
        return "journal read failed";
    }
    boot_replay_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    boot_records   = scan.records;
    boot_bytes     = clamp32((uint64_t)st.st_size);
    if (scan.crypto_error) {
        return "journal crypto failed"; // no verdict on the journal: it stays as it is
    }
    note_damage(&scan);
    log_bytes   = clamp32((uint64_t)st.st_size);
    log_records = scan.records;
    if (mesh_store_last_seq() < b.max_seq) {
        ESP_LOGW(TAG, "store seq %lu behind the journal's %lu", (unsigned long)mesh_store_last_seq(),
                 (unsigned long)b.max_seq);
    }
    uint64_t live = mesh_hist_live_bytes(&w->table, keep_n);
    compact_asked = mesh_hist_should_compact((uint64_t)st.st_size, live, scan.torn || scan.resyncs || scan.heads == 0);
    return NULL;
}

static bool boot_buffers(void) {
    hist       = heap_caps_calloc(1, sizeof(*hist), MALLOC_CAP_SPIRAM);
    uint8_t *a = psram(MESH_HIST_PENDING);
    spare      = psram(MESH_HIST_PENDING);
    if (!hist || !a || !spare) {
        free(hist);
        free(a);
        free(spare);
        hist = NULL;
        return false;
    }
    mesh_hist_init(hist, a, MESH_HIST_PENDING, &wcrypt);
    return true;
}

void mesh_journal_boot(size_t keep, void (*lock)(void), void (*unlock)(void), uint8_t const *prv, size_t prv_len) {
    keep_n    = keep;
    lock_fn   = lock;
    unlock_fn = unlock;
    // The crypto first: the badge's hardware path must give the portable code's answers, or journals written by one
    // would be unreadable by the other
    int64_t t0   = esp_timer_get_time();
    int     fail = mh_selftest();
    ESP_LOGW(TAG, "journal crypto %s: self-test %s in %lld us", mh_backend(), fail ? "FAILED" : "passed",
             (long long)(esp_timer_get_time() - t0));
    uint8_t root[32];
    bool    keyed = !fail && prv && mesh_hist_key(root, prv, prv_len);
    if (keyed) {
        mesh_hist_crypt_init(&wcrypt, root, random_fill);
    }
    mh_wipe(root, sizeof(root));
    if (!keyed) {
        turn_off(fail ? "journal crypto failed its self-test" : prv ? "journal key not derived" : "no identity key");
        return;
    }
    if (!boot_buffers()) {
        turn_off("no memory");
        return;
    }
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
    char const *why = journal_files(&w, &err);
    why             = why ? why : journal_load(&w, &err);
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
    ESP_LOGW(TAG, "message history: %lu messages loaded, journal %lu KB in %s%s", (unsigned long)hist->loaded,
             (unsigned long)(log_bytes / 1024), SD_MESSAGES_DIR, compact_asked ? ", compaction due" : "");
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
    if (!hist || !on) {
        return;
    }
    if (hist->dropped && !drop_warned) {
        drop_warned = true;
        ESP_LOGW(TAG, "pending buffer full (the card was busy or slow): messages not journaled");
    }
    if (out_len) {
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
    log_bytes = clamp32((uint64_t)log_bytes + out_len);
    log_records += out_records;
    flushes++;
    last_flush_s  = mesh_uptime_s();
    last_flush_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    out_len       = 0;
}

// On the mesh task, without the mesh lock: two passes over log.bin, so this blocks MeshCore for as long as reading the
// journal twice takes. After boot when it found damage or a journal twice its live size, on request, and past the cap.
static void compact_now(void) {
    work_t w = {0};
    if (!work_alloc(&w)) {
        work_free(&w);
        turn_off("no memory to compact the journal"); // appending to a damaged journal would hide what is appended
        return;
    }
    if (!sd_begin(FLUSH_WAIT_MS)) {
        work_free(&w);
        compact_asked = true; // the card was busy: again in RETRY_S
        retry_after_s = mesh_uptime_s() + RETRY_S;
        return;
    }
    int         err = 0;
    char const *why = compact_journal(&w, &err);
    sd_end(sd_io_from_errno(err));
    work_free(&w);
    if (why) {
        ESP_LOGW(TAG, "%s (errno %d)", why, err);
        turn_off(why);
        return;
    }
    lock_fn();
    mesh_hist_forget_chats(hist); // a dropped chat's CHAT record went with it: written again with its next message
    unlock_fn();
    if (log_bytes > MESH_HIST_CAP) {
        turn_off("journal full: the history kept is over the cap"); // compacting again would not help
    }
}

void mesh_journal_flush(void) {
    if (!hist || !on || mesh_uptime_s() < retry_after_s) {
        return;
    }
    // A compaction goes first: a journal with a torn end must be cleaned before anything is appended to it
    if (compact_asked) {
        compact_asked = false;
        compact_now();
        if (compact_asked || !on) {
            return;
        }
    }
    if (out_len) {
        write_out(FLUSH_WAIT_MS);
    }
    if (on && log_bytes > MESH_HIST_CAP) {
        compact_asked = true; // next loop
    }
}

void mesh_journal_stop_locked(void) {
    if (!hist || !on) {
        return;
    }
    mesh_hist_stop(hist, mesh_uptime_s());
    // One deadline for both writes: the update waits for this
    int64_t deadline = esp_timer_get_time() + (int64_t)STOP_WAIT_MS * 1000;
    if (out_len) {
        write_out(STOP_WAIT_MS); // an older buffer first: the journal stays in order
    }
    int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
    if (!out_len && hist->len && left_ms > 0) {
        spare = mesh_hist_swap(hist, spare, &out_len, &out_records);
        write_out((uint32_t)left_ms);
    }
    if (out_len || hist->len || hist->dropped) {
        ESP_LOGW(TAG, "message history for the update: %u bytes not written, %lu records dropped earlier",
                 (unsigned)(out_len + hist->len), (unsigned long)hist->dropped);
    }
    mh_wipe(&wcrypt, sizeof(wcrypt)); // the last write is done: MeshCore does not start again before the restart
}

// ---- console -------------------------------------------------------------------------------------------------------

void mesh_journal_console(char const *args) {
    if (strcmp(args, "compact") == 0) {
        if (!on) {
            console_reply("history is off: %s", why_off ? why_off : "?");
        } else {
            compact_asked = true;
            console_reply("compaction asked: the mesh task compacts, then flushes (\"fw mesh hist\" shows the result)");
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
    console_reply("history %s%s%s | journal %s %lu bytes, %lu records | loaded at boot %lu messages, %lu bad spot(s)",
                  on ? "on" : "off", on ? "" : ": ", on ? "" : (why_off ? why_off : "?"), LOG_PATH,
                  (unsigned long)log_bytes, (unsigned long)log_records, (unsigned long)loaded,
                  (unsigned long)resyncs);
    char last[32] = "never";
    if (flushes) {
        snprintf(last, sizeof(last), "%lu s ago", (unsigned long)(now - last_flush_s));
    }
    console_reply("pending %u bytes, %lu records (oldest %lu s) + %u bytes swapped out | %u outgoing held | "
                  "dropped %lu",
                  (unsigned)pending, (unsigned long)pending_records, (unsigned long)(pending ? now - oldest_s : 0),
                  (unsigned)out_len, (unsigned)held, (unsigned long)dropped);
    console_reply("boot replay %lu ms: %lu records, %lu bytes, %s crypto", (unsigned long)boot_replay_ms,
                  (unsigned long)boot_records, (unsigned long)boot_bytes, mh_backend());
    console_reply("flushes %lu, last %s (%lu ms) | write errors %lu | compactions %lu%s | auth failures %lu",
                  (unsigned long)flushes, last, (unsigned long)last_flush_ms, (unsigned long)write_errors,
                  (unsigned long)compactions, compact_asked ? " (one due)" : "", (unsigned long)auth_failures);
}
