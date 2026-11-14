#pragma once
// The console line's split into app prefix, command and arguments (#423), and the command-level gate (#373). Which
// forms of a command need developer mode or an origin is its sub-command table's (console_sub.h, #464), checked by
// run_command after this, before the command runs. Pure and host-tested (host_tests/dev_gate_test.c).

#include "dev_mode.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Splits line in place into the command name (returned) and its arguments (*args, leading spaces dropped). A leading
// word that is one of the n_ids app ids is the "fw <app> <command>" prefix (#423): *app is 1 + its index (else 0) and
// the name is the word after it. The result never starts with the prefix, so the gate sees the same name either way.
char *dev_gate_split(char *line, char const *const *ids, int n_ids, int *app, char const **args);

// What run_command does with a command it found: the gate comes first, so a refused line never reaches the command
// function. found is dev_mode_find_app's answer (DEV_FIND_DEV_OFF is already refused there).
typedef enum {
    DEV_GATE_RUN,    // call the command
    DEV_GATE_REFUSE, // answer the developer-mode note, call nothing
    DEV_GATE_UNKNOWN // no such command
} dev_gate_t;
dev_gate_t dev_gate_decide(dev_find_t found);

#ifdef __cplusplus
}
#endif
