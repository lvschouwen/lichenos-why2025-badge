// The crash log for apps (#72), on top of crash.c's log.txt and NVS counter; see lichen_crashlog.h
#include "lichen_crashlog.h"

#include "crash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lichen_settings.h"

#include <stdlib.h>
#include <string.h>

static char const TAG[] = "crashlog";

// crash.c keeps 20 lines of at most 256 bytes; room for a few more so an odd line never cuts the newest off
#define LOG_CAP (CRASHLOG_MAX * CRASHLOG_LINE_MAX + 1024)

// The app's seen marker: crash.c's counter and the newest line's hash when the viewer was last opened
#define KEY_SEEN_COUNT "seen_n"
#define KEY_SEEN_HASH  "seen_h"

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

static void store_seen(lichen_app_t const *app, uint32_t count, uint32_t hash) {
    app_settings_set_u32(app, KEY_SEEN_COUNT, count);
    app_settings_set_u32(app, KEY_SEEN_HASH, hash);
}

void lichen_crashlog_mark_seen(lichen_app_t const *app) {
    size_t len, entries;
    char  *buf = load(&len);
    if (buf) {
        store_seen(app, crash_count_since_clear(), newest_hash(buf, len, &entries));
    }
    free(buf);
}

bool lichen_crashlog_delete(lichen_app_t const *app, size_t index, crashlog_entry_t const *expect) {
    size_t          len;
    char           *buf = load(&len);
    crashlog_span_t spans[CRASHLOG_MAX];
    size_t          n  = buf ? crashlog_split(buf, len, spans, CRASHLOG_MAX) : 0;
    // The log may have changed under the viewer (fw crash clear): delete only the line that was shown
    bool ok = index < n && spans[index].len == strlen(expect->line) &&
              memcmp(buf + spans[index].start, expect->line, spans[index].len) == 0;
    ok = ok && crashlog_remove(buf, &len, index) && crash_log_store(buf, len);
    if (ok) {
        size_t entries;
        store_seen(app, crash_count_since_clear(), newest_hash(buf, len, &entries));
    }
    ESP_LOGW(TAG, "delete entry %u: %s", (unsigned)index, ok ? "done" : "not deleted");
    free(buf);
    return ok;
}

bool lichen_crashlog_clear(lichen_app_t const *app) {
    bool ok = crash_clear_all();
    store_seen(app, 0, 0);
    ESP_LOGW(TAG, "clear all: %s", ok ? "done" : "the core dump was not erased");
    return ok;
}

void lichen_crashlog_status(lichen_app_t const *app, lichen_crashlog_status_t *out) {
    memset(out, 0, sizeof(*out));
    size_t          len;
    char           *buf = load(&len);
    crashlog_span_t spans[CRASHLOG_MAX];
    size_t          n = buf ? crashlog_split(buf, len, spans, CRASHLOG_MAX) : 0;
    if (n) {
        crashlog_parse(buf + spans[0].start, spans[0].len, &out->newest);
        uint32_t hash = crashlog_hash(buf + spans[0].start, spans[0].len);
        out->unseen   = crashlog_unseen(crash_count_since_clear(), app_settings_get_u32(app, KEY_SEEN_COUNT, 0), hash,
                                        app_settings_get_u32(app, KEY_SEEN_HASH, 0), n);
    }
    out->entries = n;
    free(buf);
}

uint32_t lichen_crashlog_generation(void) {
    return crash_log_generation();
}

bool lichen_crashlog_after_crash(void) {
    return crash_recovered();
}
