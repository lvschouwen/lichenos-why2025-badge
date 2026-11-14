// The console's line split and command gate (#421, #423): see dev_gate.h
#include "dev_gate.h"

#include "dev_mode.h"

#include <string.h>

dev_gate_t dev_gate_decide(dev_find_t found) {
    switch (found) {
        case DEV_FIND_NONE: return DEV_GATE_UNKNOWN;
        case DEV_FIND_DEV_OFF: return DEV_GATE_REFUSE;
        case DEV_FIND_RUN: return DEV_GATE_RUN; // a form's own gate: its sub-command row (console_sub.h, #464)
    }
    return DEV_GATE_REFUSE; // fail closed
}

static char *word_end(char *s, char const **args) {
    char *rest = strchr(s, ' ');
    if (rest) {
        *rest++ = '\0';
        while (*rest == ' ') {
            rest++;
        }
    } else {
        rest = s + strlen(s);
    }
    *args = rest;
    return s;
}

char *dev_gate_split(char *line, char const *const *ids, int n_ids, int *app, char const **args) {
    char *name = word_end(line, args);
    *app       = dev_mode_app_of(ids, n_ids, name);
    if (*app) {
        name = word_end((char *)*args, args);
    }
    return name;
}
