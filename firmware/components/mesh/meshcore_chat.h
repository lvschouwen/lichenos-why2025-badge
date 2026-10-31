#pragma once

// The channel chat (#46): the channel picker, then one channel's messages with a compose line under them.
// LVGL task only; opens screens on the shell's stack, Esc goes back.
void meshcore_chat_pick(void);
// The same for a direct message (#47): pick a contact, then the DM chat. Tab retries the newest failed message.
void meshcore_chat_pick_contact(void);
