// The crash log for apps (#72), on top of crash.c's log.txt and NVS counter; see lichen_crashlog.h
#include "lichen_crashlog.h"

#include "crash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "lichen_settings.h"
#include "lichen_sys_app.h"
#include "sdcard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static char const TAG[] = "crashlog";

// crash.c keeps 20 lines of at most 256 bytes; room for a few more so an odd line never cuts the newest off
#define LOG_CAP (CRASHLOG_MAX * CRASHLOG_LINE_MAX + 1024)

#define EXPORT_CAP (CRASHLOG_MAX * (CRASHLOG_LINE_MAX + 1) + 512)

// The app's seen marker: crash.c's counter and the newest line's hash when the viewer was last opened
#define KEY_SEEN "seen"

// The log text in PSRAM (internal RAM is for the display); NULL when there is no memory. Free it.
static char *load(size_t *len) {
    char *buf = heap_caps_malloc(LOG_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "no memory for the log");
        *len = 0;
        return NULL;
    }
    *len = crash_log_load(buf, LOG_CAP);
    return buf;
}

size_t lichen_crashlog_list(crashlog_entry_t *out, size_t max) {
    size_t          len;
    char           *buf = load(&len);
    crashlog_span_t spans[CRASHLOG_MAX];
    size_t          n = buf ? crashlog_split(buf, len, spans, max < CRASHLOG_MAX ? max : CRASHLOG_MAX) : 0;
    for (size_t i = 0; i < n; i++) {
        crashlog_parse(buf + spans[i].start, spans[i].len, &out[i]);
    }
    free(buf);
    return n;
}

bool lichen_crashlog_read(size_t index, crashlog_entry_t *out) {
    size_t          len;
    char           *buf = load(&len);
    crashlog_span_t spans[CRASHLOG_MAX];
    size_t          n = buf ? crashlog_split(buf, len, spans, CRASHLOG_MAX) : 0;
    if (index < n) {
        crashlog_parse(buf + spans[index].start, spans[index].len, out);
    }
    free(buf);
    return index < n;
}

// The newest line's hash and the entry count of the log in buf
static uint32_t newest_hash(char const *buf, size_t len, size_t *entries) {
    crashlog_span_t spans[CRASHLOG_MAX];
    *entries = crashlog_split(buf, len, spans, CRASHLOG_MAX);
    return *entries ? crashlog_hash(buf + spans[0].start, spans[0].len) : 0;
}

// One NVS key, one commit: the counter in the high half, the hash in the low one
static uint64_t marker(uint32_t count, uint32_t hash) {
    return ((uint64_t)count << 32) | hash;
}

// False (nothing may be done) when ctx is refused: the context table has logged and counted it. The OS's ctx passes;
// it has no marker of its own (the settings refuse it), the log itself is the OS's to read and clear.
static bool ctx_ok(lichen_ctx_t ctx, char const *what) {
    char const *id, *name;
    return lichen_sys_ctx_app(ctx, what, &id, &name) != GATE_APP_REFUSED;
}

static void store_seen(lichen_ctx_t ctx, uint32_t count, uint32_t hash) {
    uint64_t const want = marker(count, hash);
    if (app_settings_get_u64(ctx, KEY_SEEN, UINT64_MAX) != want) { // opening the app writes nothing when nothing changed
        app_settings_set_u64(ctx, KEY_SEEN, want);
    }
}

bool lichen_crashlog_mark_seen(lichen_ctx_t ctx) {
    static bool logged;
    size_t      len, entries;
    if (!ctx_ok(ctx, "lichen_crashlog_mark_seen")) {
        return false;
    }
    char       *buf = load(&len);
    if (buf) {
        store_seen(ctx, crash_count_since_clear(), newest_hash(buf, len, &entries));
    } else if (!logged) {
        ESP_LOGW(TAG, "mark seen: log not read, the tile may count old entries as new");
        logged = true;
    }
    free(buf);
    return buf != NULL;
}

// The index-th line is still the one the viewer showed
static bool still_there(char const *buf, crashlog_span_t const *spans, size_t n, size_t index, crashlog_entry_t const *expect) {
    return expect && index < n && spans[index].len == strlen(expect->line) &&
           memcmp(buf + spans[index].start, expect->line, spans[index].len) == 0;
}

bool lichen_crashlog_delete(lichen_ctx_t ctx, size_t index, crashlog_entry_t const *expect) {
    if (!ctx_ok(ctx, "lichen_crashlog_delete")) {
        return false; // before the lock and before expect or the log are looked at
    }
    crash_log_lock(); // "fw crash clear" waits until the rewrite is done
    size_t          len;
    char           *buf = load(&len);
    crashlog_span_t spans[CRASHLOG_MAX];
    size_t          n  = buf ? crashlog_split(buf, len, spans, CRASHLOG_MAX) : 0;
    bool            ok = still_there(buf, spans, n, index, expect) && crashlog_remove(buf, &len, index) &&
              crash_log_store(buf, len);
    if (ok) {
        size_t entries;
        store_seen(ctx, crash_count_since_clear(), newest_hash(buf, len, &entries));
    }
    crash_log_unlock();
    ESP_LOG_LEVEL(
        ok ? ESP_LOG_INFO : ESP_LOG_WARN,
        TAG,
        "delete entry %u: %s",
        (unsigned)index,
        ok ? "done" : "not deleted"
    );
    free(buf);
    return ok;
}

bool lichen_crashlog_clear(lichen_ctx_t ctx) {
    if (!ctx_ok(ctx, "lichen_crashlog_clear")) {
        return false; // nothing is cleared for a ctx the OS does not know
    }
    bool ok = crash_clear_all();
    store_seen(ctx, 0, 0);
    ESP_LOG_LEVEL(ok ? ESP_LOG_INFO : ESP_LOG_WARN, TAG, "clear all: %s", ok ? "done" : "the core dump was not erased");
    return ok;
}

bool lichen_crashlog_status(lichen_ctx_t ctx, lichen_crashlog_status_t *out) {
    memset(out, 0, sizeof(*out));
    if (!ctx_ok(ctx, "lichen_crashlog_status")) {
        return false; // out->ok is false: the viewer reads it as "could not read", not as "no crashes"
    }
    size_t          len;
    char           *buf = load(&len);
    crashlog_span_t spans[CRASHLOG_MAX];
    size_t          n = buf ? crashlog_split(buf, len, spans, CRASHLOG_MAX) : 0;
    if (n) {
        crashlog_parse(buf + spans[0].start, spans[0].len, &out->newest);
        uint32_t hash = crashlog_hash(buf + spans[0].start, spans[0].len);
        uint64_t seen = app_settings_get_u64(ctx, KEY_SEEN, 0);
        out->unseen   = crashlog_unseen(crash_count_since_clear(), (uint32_t)(seen >> 32), hash, (uint32_t)seen, n);
    }
    out->entries = n;
    out->ok      = buf != NULL;
    free(buf);
    return out->ok;
}

size_t lichen_crashlog_export(char *out, size_t cap) {
    size_t len;
    char  *buf = load(&len);
    if (!buf) {
        return 0;
    }
    size_t n = crashlog_export(buf, len, esp_app_get_description()->version, out, cap);
    free(buf);
    return n;
}

bool lichen_crashlog_export_sd(char *path, size_t cap) {
    if (!sd_mounted()) {
        return false;
    }
    char  *text = heap_caps_malloc(EXPORT_CAP, MALLOC_CAP_SPIRAM);
    size_t n    = text ? lichen_crashlog_export(text, EXPORT_CAP) : 0;
    bool   ok   = false;
    char   name[48];
    crashlog_export_name((int64_t)time(NULL), crash_count_since_clear(), name, sizeof(name));
    if (n && snprintf(path, cap, "%s/%s", SD_LICHEN_DIR, name) < (int)cap && sd_begin(2000)) {
        FILE *f = fopen(path, "w");
        ok      = f && fwrite(text, 1, n, f) == n;
        ok      = (f && fclose(f) == 0) && ok;
        if (!ok) {
            ESP_LOGW(TAG, "export to the SD card failed: errno %d", errno);
        }
        sd_end(ok ? SD_IO_OK : sd_io_from_errno(errno));
    }
    free(text);
    return ok;
}

uint32_t lichen_crashlog_generation(void) {
    return crash_log_generation();
}

bool lichen_crashlog_after_crash(void) {
    return crash_recovered();
}
