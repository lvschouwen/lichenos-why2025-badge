// "fw mesh chat open|close" (#438): mesh_chat_cmd.h
#include "mesh_chat_cmd.h"

#include <string.h>

static char const *skip_spaces(char const *s) {
    while (*s == ' ') {
        s++;
    }
    return s;
}

// A word of the line: its length up to the next space
static bool word_is(char const *s, size_t len, char const *word) {
    return len == strlen(word) && strncmp(s, word, len) == 0;
}

mesh_chat_cmd_t mesh_chat_cmd_parse(char const *args, char *name, size_t cap) {
    char const *w   = skip_spaces(args);
    size_t      len = strcspn(w, " ");
    if (word_is(w, len, "close")) {
        return *skip_spaces(w + len) ? MESH_CHAT_CMD_USAGE : MESH_CHAT_CMD_CLOSE;
    }
    if (!word_is(w, len, "open")) {
        return MESH_CHAT_CMD_USAGE;
    }
    char const *t    = skip_spaces(w + len);
    size_t      tlen = strcspn(t, " ");
    if (tlen == 0 || tlen >= cap || *skip_spaces(t + tlen)) {
        return MESH_CHAT_CMD_USAGE; // no target, a name no channel has, or a second word: one channel per line
    }
    memcpy(name, t, tlen);
    name[tlen] = '\0';
    return MESH_CHAT_CMD_OPEN;
}
