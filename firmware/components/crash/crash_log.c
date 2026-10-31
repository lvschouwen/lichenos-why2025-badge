#include "crash_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TASK_NAME_MAX 16

// Appends printf text at out[pos], never past cap-1; returns the new length
static size_t append(char *out, size_t cap, size_t pos, char const *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t append(char *out, size_t cap, size_t pos, char const *fmt, ...) {
    if (pos >= cap - 1) {
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

size_t crash_elf_sha_clean(char *out, size_t cap, char const *in) {
    size_t n = 0;
    if (cap == 0) {
        return 0;
    }
    for (; in && n + 1 < cap; n++) {
        char c = in[n];
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            break;
        }
        out[n] = c;
    }
    out[n] = '\0';
    return n;
}

size_t crash_record_format(char *out, size_t cap, char const *when, char const *version, char const *elf_sha, char const *reason,
                           char const *task, uint32_t pc, uint32_t const *backtrace, size_t depth) {
    if (cap == 0) {
        return 0;
    }
    // A name from a task is not trusted to be one word: the log is parsed per line and per space
    char clean[TASK_NAME_MAX + 1];
    size_t k = 0;
    for (; task && task[k] && k < TASK_NAME_MAX; k++) {
        char c   = task[k];
        bool ok  = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        clean[k] = ok ? c : '_';
    }
    clean[k] = '\0';

    size_t pos = 0;
    out[0]     = '\0';
    pos        = append(out, cap, pos, "%s %s %s task=%s pc=0x%08x", when, version, reason, clean, (unsigned)pc);
    char sha[65];
    if (crash_elf_sha_clean(sha, sizeof(sha), elf_sha)) {
        pos = append(out, cap, pos, " elf=%s", sha);
    }
    for (size_t i = 0; i < depth; i++) {
        pos = append(out, cap, pos, "%s0x%08x", i == 0 ? " bt=" : ",", (unsigned)backtrace[i]);
    }
    return pos;
}

size_t crash_log_trim(char *text, size_t len, size_t keep_lines) {
    // Count lines; a last line without '\n' counts too
    size_t lines = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n' || i == len - 1) {
            lines++;
        }
    }
    if (lines <= keep_lines) {
        return len;
    }
    size_t drop = lines - keep_lines, start = 0;
    for (size_t i = 0; i < len && drop; i++) {
        if (text[i] == '\n') {
            start = i + 1;
            drop--;
        }
    }
    memmove(text, text + start, len - start);
    text[len - start] = '\0';
    return len - start;
}

size_t crash_b64_lines(uint8_t const *in, size_t len, char *out, size_t cap, size_t line_chars) {
    static char const A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t            chars = (len + 2) / 3 * 4;
    size_t            nl    = (line_chars && chars) ? (chars - 1) / line_chars : 0;
    if (cap < chars + nl + 1) {
        return 0;
    }
    size_t o = 0, col = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
        }
        char q[4] = {A[(v >> 18) & 63], A[(v >> 12) & 63], i + 1 < len ? A[(v >> 6) & 63] : '=', i + 2 < len ? A[v & 63] : '='};
        for (int j = 0; j < 4; j++) {
            if (line_chars && col == line_chars) {
                out[o++] = '\n';
                col      = 0;
            }
            out[o++] = q[j];
            col++;
        }
    }
    out[o] = '\0';
    return o;
}

bool crash_is_crash(crash_reset_t reason, bool fresh_dump) {
    switch (reason) {
        case CRASH_RST_PANIC:
        case CRASH_RST_INT_WDT:
        case CRASH_RST_TASK_WDT:
        case CRASH_RST_BROWNOUT: return true;
        case CRASH_RST_WDT: return fresh_dump;
        default: return false;
    }
}
