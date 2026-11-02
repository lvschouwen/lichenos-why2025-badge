#pragma once

#include "mesh_store.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Who a chat screen talks to (#126). Pure logic, no ESP headers (host_tests/mesh_chat_target_test.c).
// A chat screen gets its target once, when it is built, and keeps it until it is deleted: the compose line, Tab-retry
// and mark-read all use the screen's own target, so nothing can send to a chat other than the one on screen.

#define MESH_CHAT_TITLE_MAX (MESH_NAME_MAX + 2) // "@" + a contact name, or a channel name

typedef struct {
    mesh_chat_key_t key;
    bool            dm;
    uint8_t         peer[6];                    // DM: the contact's key prefix
    char            title[MESH_CHAT_TITLE_MAX]; // channel: the name as MeshCore has it (sent with it); DM: "@bob"
} mesh_chat_target_t;

// A channel chat, by name (case kept for sending; the key is lower-cased)
void mesh_chat_target_channel(mesh_chat_target_t *t, char const *name);
// A DM by the contact's 6-byte key prefix; the title gets an "@" when it has none ("(no name)" for an empty one)
void mesh_chat_target_dm(mesh_chat_target_t *t, uint8_t const prefix[6], char const *title);
// The same chat (same key: a channel's case does not matter, a DM is its key prefix)
bool mesh_chat_target_same(mesh_chat_target_t const *a, mesh_chat_target_t const *b);

// What opening a chat does, given the chat screen on top (NULL when the top screen is not a chat):
// KEEP it (the same chat is already showing: the draft stays), REPLACE it (another chat is showing: go back below it,
// then open), or PUSH a new chat screen (anything else is on top)
typedef enum { MESH_CHAT_OPEN_PUSH, MESH_CHAT_OPEN_KEEP, MESH_CHAT_OPEN_REPLACE } mesh_chat_open_t;
mesh_chat_open_t mesh_chat_open_how(mesh_chat_target_t const *top, mesh_chat_target_t const *want);

#ifdef __cplusplus
}
#endif
