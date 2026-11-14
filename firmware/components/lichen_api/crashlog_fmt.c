#include "crashlog_fmt.h"

#include "lichen_str.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOT "\xE2\x80\xA2" // U+2022: the badge font has no U+00B7

// Appends printf text at out[pos], never past cap-1; returns the new length
static size_t append(char *out, size_t cap, size_t pos, char const *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t append(char *out, size_t cap, size_t pos, char const *fmt, ...) {
    if (cap == 0 || pos >= cap - 1) {
        return pos;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return pos;
    }
    return ((size_t)n >= cap - pos) ? cap - 1 : pos + (size_t)n;
}

// The next non-empty line from *pos (without its '\n' or "\r\n"); false at the end of the text
static bool next_line(char const *text, size_t len, size_t *pos, crashlog_span_t *span) {
    while (*pos < len) {
        size_t start = *pos, end = start;
        while (end < len && text[end] != '\n') {
            end++;
        }
        *pos     = end + 1;
        size_t l = end - start;
        if (l && text[start + l - 1] == '\r') {
            l--;
        }
        if (l) {
            *span = (crashlog_span_t){start, l};
            return true;
        }
    }
    return false;
}

size_t crashlog_split(char const *text, size_t len, crashlog_span_t *out, size_t max) {
    crashlog_span_t span;
    size_t          total = 0, n = 0, pos = 0;
    while (next_line(text, len, &pos, &span)) {
        total++;
    }
    // The newest max lines: skip the oldest, then put the newest first
    size_t skip = total > max ? total - max : 0;
    for (pos = 0; n < max && next_line(text, len, &pos, &span);) {
        if (skip) {
            skip--;
        } else {
            out[n++] = span;
        }
    }
    for (size_t i = 0; i < n / 2; i++) {
        crashlog_span_t t = out[i];
        out[i]            = out[n - 1 - i];
        out[n - 1 - i]    = t;
    }
    return n;
}

static void parse_bt(crashlog_entry_t *out, char const *v) {
    while (*v && out->depth < CRASHLOG_BT_MAX) {
        char         *end;
        unsigned long a = strtoul(v, &end, 16);
        if (end == v) {
            break;
        }
        out->bt[out->depth++] = (uint32_t)a;
        v                     = *end == ',' ? end + 1 : end;
        if (*end != ',') {
            break;
        }
    }
}

// One "key=value" word
static void parse_field(crashlog_entry_t *out, char const *w) {
    if (strncmp(w, "task=", 5) == 0) {
        lichen_str_copy_n(out->task, sizeof(out->task), w + 5, strlen(w + 5));
    } else if (strncmp(w, "pc=", 3) == 0) {
        out->pc = (uint32_t)strtoul(w + 3, NULL, 16);
    } else if (strncmp(w, "elf=", 4) == 0) {
        lichen_str_copy_n(out->elf, sizeof(out->elf), w + 4, strlen(w + 4));
    } else if (strncmp(w, "bt=", 3) == 0) {
        parse_bt(out, w + 3);
    }
}

void crashlog_parse(char const *line, size_t n, crashlog_entry_t *out) {
    memset(out, 0, sizeof(*out));
    lichen_str_copy_n(out->line, sizeof(out->line), line, n);
    char *fields[3] = {out->when, out->version, out->reason};
    size_t const caps[3] = {sizeof(out->when), sizeof(out->version), sizeof(out->reason)};
    char  word[CRASHLOG_LINE_MAX];
    size_t words = 0;
    for (char const *p = out->line; *p;) {
        while (*p == ' ') {
            p++;
        }
        size_t l = strcspn(p, " ");
        if (l == 0) {
            break;
        }
        lichen_str_copy_n(word, sizeof(word), p, l);
        if (words < 3) {
            lichen_str_copy_n(fields[words], caps[words], word, l);
        } else {
            parse_field(out, word);
        }
        words++;
        p += l;
    }
    out->parsed = words >= 3;
    if (!out->parsed) {
        out->when[0] = out->version[0] = out->reason[0] = '\0';
    }
}

char const *crashlog_reason_text(char const *reason) {
    static char const *const map[][2] = {
        {"panic", "Panic"},       {"int_wdt", "Interrupt watchdog"}, {"task_wdt", "Task watchdog"},
        {"wdt", "Watchdog"},      {"brownout", "Brownout"},          {"other", "Other reset"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(reason, map[i][0]) == 0) {
            return map[i][1];
        }
    }
    return reason;
}

void crashlog_when_text(char const *when, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    // "YYYY-MM-DDTHH:MM:SS": date, hours and minutes
    if (strlen(when) >= 16 && when[10] == 'T') {
        snprintf(out, cap, "%.10s %.5s UTC", when, when + 11);
    } else if (when[0] == '#') {
        snprintf(out, cap, "%s (clock not set)", when);
    } else {
        snprintf(out, cap, "%s", when);
    }
}

void crashlog_row(crashlog_entry_t const *e, char *title, size_t tc, char *value, size_t vc) {
    if (vc) {
        value[0] = '\0';
    }
    if (!e->parsed) {
        snprintf(title, tc, "%s", e->line);
        return;
    }
    char when[48];
    crashlog_when_text(e->when, when, sizeof(when));
    snprintf(title, tc, "%s " DOT " %s", crashlog_reason_text(e->reason), when);
    if (vc) {
        snprintf(value, vc, "%s", e->task);
    }
}

size_t crashlog_full(crashlog_entry_t const *e, bool newest, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!e->parsed) {
        return append(out, cap, 0, "Not a crash record:\n%s", e->line);
    }
    char when[48];
    crashlog_when_text(e->when, when, sizeof(when));
    size_t pos = 0;
    pos = append(out, cap, pos, "When: %s\nReset reason: %s (%s)\n", when, crashlog_reason_text(e->reason), e->reason);
    pos = append(out, cap, pos, "Task: %s\nPC: 0x%08x\nFirmware: %s\n", e->task[0] ? e->task : "?", (unsigned)e->pc,
                 e->version);
    if (e->elf[0]) {
        pos = append(out, cap, pos, "Crashed image ELF: %s\n", e->elf);
    }
    for (size_t i = 0; i < e->depth; i++) {
        pos = append(out, cap, pos, "%s0x%08x\n", i == 0 ? "Backtrace:\n  " : "  ", (unsigned)e->bt[i]);
    }
    if (newest) {
        pos = append(out, cap, pos, "Core dump: the last one on flash (tools/badge.sh crash); it may belong to a newer crash than this entry\n");
    }
    return append(out, cap, pos, "\nAs stored:\n%s", e->line);
}

bool crashlog_remove(char *text, size_t *len, size_t index) {
    crashlog_span_t spans[CRASHLOG_MAX * 4];
    size_t          n = crashlog_split(text, *len, spans, sizeof(spans) / sizeof(spans[0]));
    if (index >= n) {
        return false;
    }
    size_t start = spans[index].start;
    size_t end   = start + spans[index].len;
    if (end < *len && text[end] == '\r') {
        end++;
    }
    if (end < *len && text[end] == '\n') {
        end++;
    }
    memmove(text + start, text + end, *len - end);
    *len -= end - start;
    text[*len] = '\0';
    return true;
}

uint32_t crashlog_hash(char const *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    }
    return h;
}

size_t crashlog_unseen(uint32_t count, uint32_t seen_count, uint32_t newest_hash, uint32_t seen_hash, size_t entries) {
    uint64_t fresh;
    if (entries == 0) {
        return 0;
    }
    if (count > seen_count) {
        fresh = count - seen_count;
    } else if (count < seen_count || newest_hash != seen_hash) {
        fresh = count; // cleared since: everything stored came after the clear
    } else {
        fresh = 0;
    }
    return fresh < entries ? (size_t)fresh : entries;
}

void crashlog_badge(size_t n, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (n == 0) {
        out[0] = '\0';
    } else if (n > CRASHLOG_BADGE_CAP) {
        snprintf(out, cap, "%u+", (unsigned)CRASHLOG_BADGE_CAP);
    } else {
        snprintf(out, cap, "%u", (unsigned)n);
    }
}

void crashlog_tile(size_t entries, size_t unseen, crashlog_entry_t const *newest, char *l1, size_t c1, char *l2, size_t c2) {
    char badge[8];
    crashlog_badge(unseen, badge, sizeof(badge));
    char const *noun = entries == 1 ? "crash" : "crashes";
    if (entries == 0) {
        snprintf(l1, c1, "No crashes stored");
    } else if (badge[0]) {
        snprintf(l1, c1, "%s new " DOT " %u %s", badge, (unsigned)entries, noun);
    } else {
        snprintf(l1, c1, "%u %s", (unsigned)entries, noun);
    }
    if (entries && newest) {
        crashlog_row(newest, l2, c2, NULL, 0);
    } else if (c2) {
        l2[0] = '\0';
    }
}

// ---- the text export (#170) ----------------------------------------------------------------------------------------

#include <time.h>

size_t crashlog_export(char const *text, size_t len, char const *version, char *out, size_t cap) {
    crashlog_span_t spans[CRASHLOG_MAX];
    size_t          total = crashlog_split(text, len, spans, CRASHLOG_MAX);
    // Which lines fit: each costs its length plus '\n'; the header is written once the count is known
    char   head[200];
    size_t room = cap > sizeof(head) ? cap - sizeof(head) : 0, shown = 0, used = 0;
    while (shown < total && used + spans[shown].len + 1 < room) {
        used += spans[shown].len + 1;
        shown++;
    }
    int n = snprintf(head, sizeof(head),
                     "# Lichen crash report\n# firmware: %s\n# records: %u of %u stored, newest first\n"
                     "# line: when version reason task pc [elf] [bt]; decode with addr2line on the ELF of that version\n",
                     version && version[0] ? version : "unknown", (unsigned)shown, (unsigned)total);
    if (cap == 0 || n < 0 || (size_t)n >= sizeof(head) || (size_t)n + used >= cap) {
        if (cap) {
            out[0] = '\0';
        }
        return 0;
    }
    size_t pos = (size_t)n;
    memcpy(out, head, pos);
    for (size_t i = 0; i < shown; i++) {
        for (size_t k = 0; k < spans[i].len; k++) {
            unsigned char c = (unsigned char)text[spans[i].start + k];
            out[pos++]      = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return pos;
}

void crashlog_export_name(int64_t now, uint32_t count, char *out, size_t cap) {
    if (now > 1700000000) { // the clock is set only once Wi-Fi time arrived (as crash.c decides)
        time_t    t = (time_t)now;
        struct tm tm;
        if (gmtime_r(&t, &tm)) {
            snprintf(out, cap, "crashes-%04d%02d%02d-%02d%02d%02d.txt", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec);
            return;
        }
    }
    snprintf(out, cap, "crashes-n%u.txt", (unsigned)count);
}
