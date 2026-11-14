#pragma once
// "fw mesh chat open <#channel>" / "fw mesh chat close" (#438): the parse, pure
// (host_tests/mesh_chat_cmd_test.c). The command opens a chat screen from the console so chat-screen bench checks run
// remotely; it never sends or fills the compose field. The on-air allow list is checked by the caller
// (mesh_onair.h), the live channel list by the caller too: a contact or an unknown name is refused there, as no channel.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_CHAT_CMD_NAME_MAX 32 // a channel name with its terminator (MESH_CHANNEL_MAX + 1)

typedef enum {
    MESH_CHAT_CMD_OPEN,    // a channel name was parsed into name
    MESH_CHAT_CMD_CLOSE,
    MESH_CHAT_CMD_USAGE,   // anything else: no word, an unknown word, a missing, extra or too long argument
} mesh_chat_cmd_t;

// args: the text after "chat" (spaces around words ignored). On OPEN, name holds the target as typed.
mesh_chat_cmd_t mesh_chat_cmd_parse(char const *args, char *name, size_t cap);

#ifdef __cplusplus
}
#endif
