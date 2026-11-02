#pragma once

#include <stdint.h>

// Log in, Status and CLI for a repeater or room (#51), opened from the contact's details (meshcore_contact.c). LVGL
// task only; each opens on the shell's stack, Esc goes back. The state machine and the text are mesh_remote.h; the
// request goes to the mesh task through mesh_remote_start, and only from an Enter of the person:
//  - login: the password entry's Enter (the entry starts with the remembered password; it is remembered once the
//    repeater accepted it, kept under the contact's key in the app's settings, and never shown or logged)
//  - cli: the command entry's Enter
//  - status: the Status row's Enter (the screen asks as it opens, the Again row asks again)
void meshcore_remote_login(uint8_t const prefix[6], char const *title);
void meshcore_remote_status(uint8_t const prefix[6], char const *title);
void meshcore_remote_cli(uint8_t const prefix[6], char const *title);
