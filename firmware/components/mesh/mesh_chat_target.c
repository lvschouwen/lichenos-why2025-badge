#include "mesh_chat_target.h"

#include <stdio.h>
#include <string.h>

void mesh_chat_target_channel(mesh_chat_target_t *t, char const *name) {
    memset(t, 0, sizeof(*t));
    snprintf(t->title, sizeof(t->title), "%s", name ? name : "");
    mesh_chat_key_channel(&t->key, t->title);
}

void mesh_chat_target_dm(mesh_chat_target_t *t, uint8_t const prefix[6], char const *title) {
    memset(t, 0, sizeof(*t));
    char const *name = title && title[0] && strcmp(title, "@") != 0 ? title : "(no name)";
    snprintf(t->title, sizeof(t->title), "%s%s", name[0] == '@' ? "" : "@", name);
    t->dm = true;
    memcpy(t->peer, prefix, sizeof(t->peer));
    mesh_chat_key_dm(&t->key, t->peer);
}

bool mesh_chat_target_same(mesh_chat_target_t const *a, mesh_chat_target_t const *b) {
    return memcmp(&a->key, &b->key, sizeof(a->key)) == 0; // both keys are memset before they are filled
}

mesh_chat_open_t mesh_chat_open_how(mesh_chat_target_t const *top, mesh_chat_target_t const *want) {
    if (!top) {
        return MESH_CHAT_OPEN_PUSH;
    }
    return mesh_chat_target_same(top, want) ? MESH_CHAT_OPEN_KEEP : MESH_CHAT_OPEN_REPLACE;
}

mesh_chats_show_t mesh_chats_show_how(int list_depth) {
    if (list_depth < 0) {
        return MESH_CHATS_SHOW_OPEN;
    }
    return list_depth == 0 ? MESH_CHATS_SHOW_KEEP : MESH_CHATS_SHOW_BACK;
}
