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
// "fw mesh chat open|close" (#438), from the console task: a short MeshCore task queues the open or the close on the LVGL
// task (lichen_ui_call, so the screen is MeshCore's, #231) and this waits for its answer (up to 3 s). name NULL leaves
// the chat on top (as Esc would, also with a draft); else opens that channel's chat through meshcore_chat_open_channel.
// Never opens a DM and never sends. The compose line takes the focus as it does for a hand-opened chat; an open chat
// keeps its draft.
typedef enum {
    MESHCORE_CHAT_CON_PENDING, // while the job runs (never returned)
    MESHCORE_CHAT_CON_OK,      // done (open: the chat is on top)
    MESHCORE_CHAT_CON_FAILED,  // open: the chat is not on top after the open (no memory, a pop not queued)
    MESHCORE_CHAT_CON_NONE,    // close: no chat is on top
    MESHCORE_CHAT_CON_REFUSED, // close: the screen stayed (a dialog or the update screen refused the pop)
    MESHCORE_CHAT_CON_BUSY,    // not started: the last job still runs, or no memory or UI lock
    MESHCORE_CHAT_CON_LATE,    // no answer within 3 s: the job may still run
} meshcore_chat_con_t;
#ifdef __cplusplus
extern "C" {
#endif
meshcore_chat_con_t meshcore_chat_console(char const *name);
#ifdef __cplusplus
}
#endif
#endif
