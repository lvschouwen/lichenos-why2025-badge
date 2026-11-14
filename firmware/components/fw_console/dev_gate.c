// Which console forms need developer mode (#421): see dev_gate.h
#include "dev_gate.h"

#include "dev_mode.h"

#include <string.h>

// "notify test ..." and "vib test ...": the other forms are settings a person uses, so only the test word is gated
static bool first_word_is(char const *args, char const *word) {
    size_t n = strlen(word);
    return strncmp(args, word, n) == 0 && (args[n] == '\0' || args[n] == ' ');
}

bool dev_gate_needs_dev(char const *name, char const *args) {
    if (strcmp(name, "notify") == 0 || strcmp(name, "vib") == 0) {
        return first_word_is(args, "test");
    }
    if (strcmp(name, "mesh") == 0) {
        // #438: "mesh chat open|close" opens a screen from the console; #455: "mesh ident simulate" blocks TX
        return first_word_is(args, "chat") || first_word_is(args, "ident");
    }
    return false; // every other command's forms: its sub-command table (#464, console_sub.h)
}

dev_gate_t dev_gate_decide(dev_find_t found, bool needs_dev, bool dev_on) {
    switch (found) {
        case DEV_FIND_NONE: return DEV_GATE_UNKNOWN;
        case DEV_FIND_DEV_OFF: return DEV_GATE_REFUSE;
        case DEV_FIND_RUN: return needs_dev && !dev_on ? DEV_GATE_REFUSE : DEV_GATE_RUN;
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
