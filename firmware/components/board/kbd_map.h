#pragma once

#include <stdbool.h>
#include <stdint.h>

// The badge keyboard (TCA8418) for LVGL (#61). Pure logic, no ESP or LVGL headers: also built into the host tests.

// LVGL's key values (lv_group.h); keyboard.c checks they still match
#define KBD_KEY_UP        17
#define KBD_KEY_DOWN      18
#define KBD_KEY_RIGHT     19
#define KBD_KEY_LEFT      20
#define KBD_KEY_ESC       27
#define KBD_KEY_DEL       127
#define KBD_KEY_BACKSPACE 8
#define KBD_KEY_ENTER     10
#define KBD_KEY_NEXT      9

// key: a TCA8418 key code without its press bit (1-0x50). Returns an LVGL key (KBD_KEY_*) or a character, 0 for none.
uint32_t kbd_map_key(uint8_t key, bool shift);
// Whether key is one of the Shift keys
bool     kbd_map_is_shift(uint8_t key);
// The Left Shift key's code: what an injected uppercase character holds down (#375)
#define KBD_CODE_SHIFT 0x29
// The other way round (#375): the code that types key (a KBD_KEY_* or a character), with *shift set when Shift must be
// down for it (only then: a key both ways gives the unshifted code). 0 for none, e.g. a character no key types.
uint8_t  kbd_map_code_for(uint32_t key, bool *shift);
