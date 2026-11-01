#pragma once

#include <stdint.h>

// The chat screens (#46, #47): one channel's or one contact's messages with a compose line under them. LVGL task only;
// they open on the shell's stack, Esc goes back. Tab in a DM retries the newest failed message.
// "New message..." on the Chat list (#88): the contact picker (type to search, #57), then the DM chat, for a companion
// that has no conversation yet
void meshcore_chat_pick_contact(void);
// Open a chat straight from the conversation list (#48). The channel is named (as MeshCore has it, case does not
// matter); the DM by the contact's 6-byte key prefix and its title ("@bob", the @ is added when missing).
void meshcore_chat_open_channel(char const *name);
void meshcore_chat_open_dm(uint8_t const prefix[6], char const *title);
