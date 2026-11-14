#pragma once

#include <stdint.h>

// The chat screens (#46, #47): one channel's or one contact's messages with a compose line under them. LVGL task only;
// they open on the shell's stack, Esc goes back. Tab in a DM retries the newest failed message.
// "New message..." on the Chat list (#88): the contact picker (type to search, #57), then the DM chat, for a companion
// that has no conversation yet
void meshcore_chat_pick_contact(void);
// Open a chat straight from the conversation list (#48). The channel is named (as MeshCore has it, case does not
// matter); the DM by the contact's 6-byte key prefix and its title ("@bob", the @ is added when missing).
// Each chat screen keeps its own target for good (#126). Opening the chat that is showing does nothing (the draft
// stays); opening another while a chat is showing replaces that one (Esc then goes where it went).
void meshcore_chat_open_channel(char const *name);
void meshcore_chat_open_dm(uint8_t const prefix[6], char const *title);

#ifdef CONFIG_LICHEN_DEV_COMMANDS
// "fw mesh chat open|close" (#438), from the console task: the open or the close runs on the LVGL task through the UI
// service (lichen_ui_call) and this waits for its answer (up to 3 s). name NULL closes the chat on top (as Esc would
// leave it); else opens that channel's chat through meshcore_chat_open_channel. Never opens a DM, never touches the
// compose line, never sends.
typedef enum {
    MESHCORE_CHAT_CON_OK,   // done
    MESHCORE_CHAT_CON_NONE, // close: no chat is on top
    MESHCORE_CHAT_CON_BUSY, // not queued, or the answer did not come in time
} meshcore_chat_con_t;
#ifdef __cplusplus
extern "C" {
#endif
meshcore_chat_con_t meshcore_chat_console(char const *name);
#ifdef __cplusplus
}
#endif
#endif
