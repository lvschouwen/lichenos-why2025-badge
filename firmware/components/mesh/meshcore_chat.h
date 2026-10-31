#pragma once

#include <stdint.h>

// The channel chat (#46): the channel picker, then one channel's messages with a compose line under them.
// LVGL task only; opens screens on the shell's stack, Esc goes back.
void meshcore_chat_pick(void);
// The same for a direct message (#47): pick a contact, then the DM chat. Tab retries the newest failed message.
void meshcore_chat_pick_contact(void);
// Open a chat straight from the conversation list (#48). The channel is named (as MeshCore has it, case does not
// matter); the DM by the contact's 6-byte key prefix and its title ("@bob", the @ is added when missing).
void meshcore_chat_open_channel(char const *name);
void meshcore_chat_open_dm(uint8_t const prefix[6], char const *title);
