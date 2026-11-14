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
// A channel send passes its slot's key id (mesh_channel_key_id), resolved under the mesh lock.
char const *mesh_onair_refusal(bool from_console, onair_tx_t tx, char const *channel, uint8_t const *key_id,
                               uint8_t const *prefix);
// Whether the setting is on now (any task): the senders resolve a channel's key only then
bool        mesh_onair_on(void);
#ifdef CONFIG_LICHEN_DEV_COMMANDS
// #438: why "fw mesh chat open <channel>" (a stored name) may not open that channel's chat, NULL when it may: the same
// check a console send to it gets (the list is on and the channel is not on it, or MeshCore is busy). Console task.
char const *mesh_onair_open_refusal(char const *channel);
#endif
// mesh_api_channels.cpp: the channel a name means (typed: as "fw mesh send" resolves a typed name; else the stored
// name exactly, without case), its stored name into name and its key id (never the secret), under the mesh lock.
// 1 when found, 0 when not, MESH_BUSY when MeshCore is not running or busy.
size_t      mesh_channel_key_id(char const *channel, bool typed, char *name, size_t cap, uint8_t key_id[ONAIR_KEY_ID]);
// "fw onair [on | off | allow <channel|key digits> | remove <...> | clear]"
void mesh_onair_console(char const *args);

#ifdef __cplusplus
}
#endif
