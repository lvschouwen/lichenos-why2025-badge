// Which console forms need developer mode (#421): see dev_gate.h
#include "dev_gate.h"

#include "dev_mode.h"

#include <string.h>

// A command with test arms: "open" lists the argument strings that stay open, exactly (no open form: none listed).
// An argument not listed is gated, so a form added to a command later is gated until it is listed here.
typedef struct {
    char const *name;
    char const *open[4];
} gated_t;

static gated_t const GATED[] = {
    {"noscreen", {NULL}},                // the next boot skips the display
    {"thermal", {"", NULL}},             // "test ..." can power the badge off; "seen reset" erases a flag
    {"psram", {"", NULL}},               // test, dma, dl, dlboot and speed change timing or the next boot
    {"stress", {"status", NULL}},        // a bare "stress" starts a run
    {"idle", {"", NULL}},                // "test <s>" fakes the idle clock
    {"setup", {NULL}},                   // reset and done write the first-run flags
    {"sensors", {"", "raw", "boot", NULL}}, // start, stop and "boot on|off" change state and what boots
};

// "notify test ..." and "vib test ...": the other forms are settings a person uses, so only the test word is gated
static bool first_word_is(char const *args, char const *word) {
    size_t n = strlen(word);
    return strncmp(args, word, n) == 0 && (args[n] == '\0' || args[n] == ' ');
}

bool dev_gate_needs_dev(char const *name, char const *args) {
    if (strcmp(name, "notify") == 0 || strcmp(name, "vib") == 0) {
        return first_word_is(args, "test");
    }
    for (unsigned i = 0; i < sizeof(GATED) / sizeof(GATED[0]); i++) {
        if (strcmp(name, GATED[i].name) != 0) {
            continue;
        }
        for (int o = 0; GATED[i].open[o]; o++) {
            if (strcmp(args, GATED[i].open[o]) == 0) {
                return false;
            }
        }
        return true;
    }
    return false;
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
