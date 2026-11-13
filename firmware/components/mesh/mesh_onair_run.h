#pragma once
// The console's on-air allow list on the badge (#289): the live copy, its setting and "fw onair". The rules are
// mesh_onair.h's (host-tested).
#include "mesh_onair.h"

#ifdef __cplusplus
extern "C" {
#endif

// Once, from mesh_console_init (boot, before any console command or MeshCore start): makes the lock, loads the setting
void mesh_onair_init(void);
// NULL when the send may go on: always for a screen's (from_console false) and while the setting is off. Else why
// not: "on air refused: <target> is not on the console allow list" on a console command's task (its buffer is only
// written by console runs, which console.c serialises), ONAIR_REFUSED_POLL on any other (the mesh task's polls).
char const *mesh_onair_refusal(bool from_console, onair_tx_t tx, char const *channel, uint8_t const *prefix);
// "fw onair [on | off | allow <channel|key digits> | remove <...> | clear]"
void mesh_onair_console(char const *args);

#ifdef __cplusplus
}
#endif
