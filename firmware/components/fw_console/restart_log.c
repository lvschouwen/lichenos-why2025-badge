#include "restart_log.h"

#include <stdio.h>
#include <string.h>

size_t restart_line_format(char *out, size_t cap, char const *when, char const *version, uint32_t uptime_s, char const *why,
                           uint32_t caller) {
    char   clean[48];
    size_t n = 0;
    for (char const *p = why ? why : "?"; *p && n < sizeof(clean) - 1; p++) {
        clean[n++] = (*p >= 0x20 && *p < 0x7f) ? *p : '_';
    }
    clean[n] = '\0';
    int len  = snprintf(out, cap, "%s %s up=%us %s", when ? when : "t?", version ? version : "?", (unsigned)uptime_s, clean);
    if (len > 0 && caller && (size_t)len < cap) {
        snprintf(out + len, cap - (size_t)len, " caller=0x%08x", (unsigned)caller);
    }
    return strlen(out);
}

void restart_log_push(restart_log_t *log, char const *line) {
    memmove(log->line[1], log->line[0], sizeof(log->line) - sizeof(log->line[0]));
    strncpy(log->line[0], line, RESTART_LINE_MAX - 1);
    log->line[0][RESTART_LINE_MAX - 1] = '\0';
    if (log->count < RESTART_LOG_KEEP) {
        log->count++;
    }
}

void restart_log_sanitize(restart_log_t *log) {
    if (log->count > RESTART_LOG_KEEP) {
        log->count = RESTART_LOG_KEEP;
    }
    for (int i = 0; i < RESTART_LOG_KEEP; i++) {
        log->line[i][RESTART_LINE_MAX - 1] = '\0';
    }
}
