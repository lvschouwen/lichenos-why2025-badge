#pragma once

#include "mesh_notify.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MeshCore's side of the notification service (#50): the events from the feed, the settings, the open chat. The
// decisions are mesh_notify.c's. Every function is safe from any task; the push functions may run under the mesh lock
// (a spinlock only), the drain never does.

// ---- events, from the mesh task under the mesh lock ----------------------------------------------------------------
void mesh_notify_dm(mesh_chat_key_t const *key, char const *from_title, char const *text);
void mesh_notify_channel(mesh_chat_key_t const *key, char const *channel_name, char const *text);
// An own message ended FAILED; title is "@name" or the channel name
void mesh_notify_failed(mesh_chat_key_t const *key, char const *title, char const *text);
// True once per message when its state has just become FAILED (a retry that fails again counts again): the caller then
// builds the note with mesh_notify_failed. muted: the stop on request fails what was waiting, which the user asked for
bool mesh_notify_out_state(uint32_t id, bool failed);
void mesh_notify_mute_failures(bool muted);

// ---- the mesh task, after the unlock ---------------------------------------------------------------------------------
// Posts what waits. true when the home badge must be counted again (a mode was read, or a setting changed)
bool mesh_notify_drain(void);
// For the badge: does this chat's unread count? Reads the cache only (no storage): the mesh lock may be held
bool mesh_notify_counts_unread(mesh_chat_key_t const *key, void *ctx);

// ---- the open chat (LVGL task) -----------------------------------------------------------------------------------------
// The chat is on screen: nothing is posted for it and its notices are marked read. clear only when key is still the
// open one (the next chat may have loaded before this one unloaded).
void mesh_notify_set_open_chat(mesh_chat_key_t const *key);
void mesh_notify_clear_open_chat(mesh_chat_key_t const *key);

// ---- settings (namespace a.meshcore) ----------------------------------------------------------------------------------
typedef enum { MN_SW_DM, MN_SW_FAIL, MN_SW_MENTIONS } mn_switch_t;
bool      mesh_notify_get_switch(mn_switch_t which);
bool      mesh_notify_set_switch(mn_switch_t which, bool on);
mn_mode_t mesh_notify_chan_mode(char const *channel_name);
bool      mesh_notify_set_chan_mode(char const *channel_name, mn_mode_t mode);
char const *mn_mode_name(mn_mode_t mode);

// "fw mesh notify ..." (the part after "notify")
void mesh_notify_console(char const *args);

#ifdef __cplusplus
}
#endif
