#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The crash log viewer's text (#72). Pure, no ESP headers; host-tested in host_tests/crashlog_fmt_test.c.
// A log line is what crash_record_format() (crash/crash_log.h) writes:
//   "<when> <version> <reason> task=<task> pc=0x%08x [elf=<sha>] [bt=0x..,0x..]"
// with <when> "2026-10-30T12:00:00" (UTC) or "#<n>" when the clock was not set yet.

#define CRASHLOG_MAX       20 // crash.c keeps the last 20
#define CRASHLOG_LINE_MAX  256
#define CRASHLOG_BT_MAX    16
#define CRASHLOG_BADGE_CAP 99 // "99+" above this

typedef struct {
    char     line[CRASHLOG_LINE_MAX]; // as stored (cut at CRASHLOG_LINE_MAX - 1)
    bool     parsed;                  // false: not a crash record; only line is set
    char     when[24];
    char     version[48];
    char     reason[16]; // panic, int_wdt, task_wdt, wdt, brownout, other
    char     task[17];
    char     elf[17]; // the crashed image's ELF SHA prefix, "" when not recorded
    uint32_t pc;
    uint32_t bt[CRASHLOG_BT_MAX];
    size_t   depth;
} crashlog_entry_t;

typedef struct {
    size_t start, len;
} crashlog_span_t;

// The non-empty lines of log text, newest (the last line) first; returns how many were stored (max at most)
size_t crashlog_split(char const *text, size_t len, crashlog_span_t *out, size_t max);
// One line of n bytes (no '\n'); out->parsed says whether it was a crash record (three words at least)
void crashlog_parse(char const *line, size_t n, crashlog_entry_t *out);

// "Panic", "Task watchdog", ...; the reason itself when it is not one of crash.c's
char const *crashlog_reason_text(char const *reason);
// "2026-10-30 12:00 UTC", or "#3 (clock not set)"
void crashlog_when_text(char const *when, char *out, size_t cap);
// A list row: title "Panic • 2026-10-30 12:00 UTC", value the task ("console"); an unparsed line is its own title
void crashlog_row(crashlog_entry_t const *e, char *title, size_t tc, char *value, size_t vc);
// The full view, one fact per line; newest adds where the core dump is. Returns the length (NUL terminated).
size_t crashlog_full(crashlog_entry_t const *e, bool newest, char *out, size_t cap);

// Removes the index-th non-empty line counted newest first (as crashlog_split numbers them) from text in place, with
// its '\n'; *len is updated. false (nothing changes) when there is no such line.
bool crashlog_remove(char *text, size_t *len, size_t index);

// FNV-1a over n bytes: names the newest entry in the seen marker
uint32_t crashlog_hash(char const *s, size_t n);
// Entries new since the viewer was last opened. count is crash.c's NVS counter (it only grows until a clear),
// seen_count and seen_hash what it and the newest line's hash were when the viewer was last opened. A counter below
// seen_count was cleared since (fw crash clear), as is an equal one under a different newest line: then everything
// stored is new. Never more than entries.
size_t crashlog_unseen(uint32_t count, uint32_t seen_count, uint32_t newest_hash, uint32_t seen_hash, size_t entries);
// "" for none, "7", "99+"
void crashlog_badge(size_t n, char *out, size_t cap);
// The home tile: line1 "2 new • 5 crashes" ("5 crashes", "No crashes stored"), line2 the newest entry's row title
void crashlog_tile(size_t entries, size_t unseen, crashlog_entry_t const *newest, char *l1, size_t c1, char *l2, size_t c2);

#ifdef __cplusplus
}
#endif
