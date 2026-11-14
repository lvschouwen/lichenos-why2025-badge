#include "c6_log_arg.h"

bool c6_log_parse_ms(char const *args, uint32_t *ms) {
    if (!*args) {
        *ms = C6_LOG_DEFAULT_MS;
        return true;
    }
    while (*args == ' ' || *args == '\t') {
        args++;
    }
    if (*args < '0' || *args > '9') {
        return false;
    }
    uint32_t v = 0;
    for (; *args >= '0' && *args <= '9'; args++) {
        v = v * 10 + (uint32_t)(*args - '0');
        if (v > C6_LOG_MAX_MS) { // also stops a long digit run from overflowing
            return false;
        }
    }
    if (*args || v == 0) { // anything after the digits, or 0
        return false;
    }
    *ms = v;
    return true;
}
