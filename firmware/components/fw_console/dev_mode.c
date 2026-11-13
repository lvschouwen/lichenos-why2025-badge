// Developer mode's rules (#373): see dev_mode.h
#include "dev_mode.h"

#include <string.h>

dev_find_t dev_mode_find(console_entry_t const *table, int n, char const *name, bool on, int *index) {
    for (int i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) {
            *index = i;
            return table[i].dev && !on ? DEV_FIND_DEV_OFF : DEV_FIND_RUN;
        }
    }
    return DEV_FIND_NONE;
}

int dev_mode_help(console_entry_t const *table, int n, bool on, void (*line)(char const *usage, void *ctx), void *ctx) {
    int listed = 0;
    for (int i = 0; i < n; i++) {
        if (!table[i].dev || on) {
            line(table[i].usage, ctx);
            listed++;
        }
    }
    return listed;
}

console_origin_t dev_mode_exec_origin(bool keep_secret) {
    return keep_secret ? CONSOLE_ORIGIN_WIFI : CONSOLE_ORIGIN_LOCAL;
}

console_origin_t dev_mode_origin(bool captured, console_origin_t exec, bool uart_task) {
    if (captured) {
        return exec;
    }
    return uart_task ? CONSOLE_ORIGIN_UART : CONSOLE_ORIGIN_UNKNOWN;
}

bool console_hold_refuses(bool held, console_origin_t origin) {
    return held && origin != CONSOLE_ORIGIN_UART;
}

dev_req_t dev_mode_request(char const *args, console_origin_t origin) {
    if (args[0] == '\0' || strcmp(args, "status") == 0) {
        return DEV_REQ_STATUS;
    }
    if (strcmp(args, "off") == 0) {
        return DEV_REQ_OFF;
    }
    if (strcmp(args, "on") == 0) {
        return origin == CONSOLE_ORIGIN_UART ? DEV_REQ_ON : DEV_REQ_REFUSED;
    }
    return DEV_REQ_USAGE;
}
