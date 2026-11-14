// Developer mode's rules (#373): see dev_mode.h
#include "dev_mode.h"

#include <stdio.h>
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

int dev_mode_app_of(char const *const *ids, int n, char const *word) {
    for (int i = 0; i < n; i++) {
        if (strcmp(ids[i], word) == 0) {
            return i + 1;
        }
    }
    return 0;
}

int dev_mode_app_intern(char const **ids, int *n, int max, char const *id) {
    int known = dev_mode_app_of(ids, *n, id);
    if (known || *n >= max) {
        return known;
    }
    ids[*n] = id;
    __atomic_store_n(n, *n + 1, __ATOMIC_RELEASE); // readers load the count without the lock (console.c)
    return *n;
}

bool dev_mode_name_clash(console_entry_t const *table, int n, char const *const *ids, int n_ids, char const *app,
                         char const *name) {
    int unused;
    if (dev_mode_app_of(ids, n_ids, name) != 0) {
        return true;
    }
    return app != NULL && (strcmp(app, name) == 0 || dev_mode_find(table, n, app, true, &unused) != DEV_FIND_NONE);
}

dev_find_t dev_mode_find_app(console_entry_t const *table, int n, char const *name, bool on, int app, int *index) {
    dev_find_t r = dev_mode_find(table, n, name, on, index);
    if (r != DEV_FIND_NONE && app != 0 && table[*index].app != app) {
        return DEV_FIND_NONE;
    }
    return r;
}

int dev_mode_help_app(console_entry_t const *table, int n, bool on, int app, void (*line)(char const *usage, void *ctx),
                      void *ctx) {
    int listed = 0;
    for (int i = 0; i < n; i++) {
        if (table[i].app == app && (!table[i].dev || on)) {
            if (line) {
                line(table[i].usage, ctx);
            }
            listed++;
        }
    }
    return listed;
}

int dev_mode_help_names(help_view_t const *v, int app, char const *head, size_t width,
                        void (*line)(char const *text, void *ctx), void *ctx) {
    console_entry_t const *table = v->table;
    char   buf[DEV_HELP_LINE_MAX];
    size_t head_len = strlen(head);
    if (head_len >= sizeof(buf)) {
        return 0; // a caller's bug: print nothing rather than a cut line
    }
    if (width >= sizeof(buf)) {
        width = sizeof(buf) - 1;
    }
    memcpy(buf, head, head_len + 1);
    size_t len     = head_len;
    bool   on_line = false; // a name is on the current line
    bool   bare    = false; // the line is a continuation: two blanks, then the first name without a separator
    int    listed  = 0;
    for (int i = 0; i < v->n; i++) {
        if (table[i].app != app || (table[i].dev && !v->on)) {
            continue;
        }
        size_t nl = strlen(table[i].name);
        if (on_line && len + 1 + nl > width) {
            line(buf, ctx);
            len     = (size_t)snprintf(buf, sizeof(buf), "  ");
            on_line = false;
            bare    = true;
        }
        size_t put = (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%s", bare && !on_line ? "" : " ", table[i].name);
        len += put < sizeof(buf) - len ? put : sizeof(buf) - len - 1; // a name longer than a line is cut, still listed
        on_line = true;
        listed++;
    }
    if (listed) {
        line(buf, ctx);
    }
    return listed;
}

int dev_mode_help_topic(help_view_t const *v, char const *const *ids, int n_ids, char const *word,
                        void (*line)(char const *usage, void *ctx), void *ctx) {
    int app = dev_mode_app_of(ids, n_ids, word);
    if (app) {
        return dev_mode_help_app(v->table, v->n, v->on, app, line, ctx);
    }
    int index;
    if (dev_mode_find(v->table, v->n, word, v->on, &index) != DEV_FIND_RUN) {
        return 0;
    }
    line(v->table[index].usage, ctx);
    return 1;
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

bool console_erase_refuses(console_origin_t origin) {
    return origin != CONSOLE_ORIGIN_UART;
}

slot_req_t console_slot_request(char const *args, bool held, console_origin_t origin) {
    if (args[0] == '\0') {
        return SLOT_REQ_SHOW;
    }
    if (strcmp(args, "other") != 0) {
        return SLOT_REQ_USAGE;
    }
    return console_hold_refuses(held, origin) ? SLOT_REQ_REFUSED : SLOT_REQ_OTHER;
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
