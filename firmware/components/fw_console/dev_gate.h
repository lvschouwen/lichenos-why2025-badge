#pragma once
// Which console command lines are test or state-changing arms that run only in developer mode (#421), decided in one
// place from a table instead of in each command. console.c asks before it calls a command, so the gate sits before
// any state is written and a new spelling of the line cannot skip it. Pure and host-tested (host_tests/dev_gate_test.c).

#include "dev_mode.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Splits line in place into the command name (returned) and its arguments (*args, leading spaces dropped). A leading
// word that is one of the n_ids app ids is the "fw <app> <command>" prefix (#423): *app is 1 + its index (else 0) and
// the name is the word after it. The result never starts with the prefix, so the gate sees the same name either way.
char *dev_gate_split(char *line, char const *const *ids, int n_ids, int *app, char const **args);

// True when "name args" is a form that needs developer mode. Fail closed: a command with a gated form lists the forms
// that stay open (a status print), and any other argument is gated, including one the command itself rejects.
bool dev_gate_needs_dev(char const *name, char const *args);

// What run_command does with a command it found: the gate comes first, so a refused line never reaches the command
// function. found is dev_mode_find_app's answer (DEV_FIND_DEV_OFF is already refused there).
typedef enum {
    DEV_GATE_RUN,    // call the command
    DEV_GATE_REFUSE, // answer the developer-mode note, call nothing
    DEV_GATE_UNKNOWN // no such command
} dev_gate_t;
dev_gate_t dev_gate_decide(dev_find_t found, bool needs_dev, bool dev_on);

#ifdef __cplusplus
}
#endif
