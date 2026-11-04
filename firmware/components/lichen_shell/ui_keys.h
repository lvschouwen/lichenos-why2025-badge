#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// `fw keys send <seq>` (bench diagnostic): the key sequence tokenizer. Pure, host-tested in host_tests/ui_keys_test.c.
// Tokens are separated by spaces: up down left right enter esc tab bksp wait, the shape keys (#79) square triangle
// cross circle cloud diamond, or text:<characters> (everything up to the next '|' or the end of the string, spaces
// included, so "text:Ab c|enter" types Ab c and presses Enter). wait is a pause (UI_KEY_WAIT), not a key.
#define UI_KEY_WAIT UINT32_MAX
// A shape key is no LVGL key: it goes to the OS shortcuts as its TCA8418 code (2-7) would
#define UI_KEY_SHAPE(code)    (0x80000000u | (uint32_t)(code))
#define UI_KEY_IS_SHAPE(k)    ((k) != UI_KEY_WAIT && ((k) & 0x80000000u) != 0)
#define UI_KEY_SHAPE_CODE(k)  ((uint8_t)((k) & 0xffu))
#define UI_KEYS_ERR ((size_t)-1)
// Focus guards (#190), so a whole route can go in one command and Enter is still only pressed on the row meant:
// focus:<text> (the focused row contains text) and row:<text> (it is text, a leading "* " ignored), text up to the next
// '|' or the end like text:. A guard that fails stops the sequence there; no later key is sent. No focus, or "(none)",
// never passes. The value holds where the text is in the sequence string (ui_keys_guard_ok reads it from there).
// notfocus:<a;b;...> passes only on a focused row that contains none of the ';'-separated texts (the sweep's rows that
// switch, send or erase, #190). needlock: the remote key lock must be on here and stay on to the end of the sequence.
#define UI_KEY_GUARD_BIT      0x40000000u
#define UI_KEY_GUARD_EXACT    0x01000000u
#define UI_KEY_GUARD_NOT      0x02000000u
#define UI_KEY_NEEDLOCK       0x20000000u
#define UI_KEY_IS_GUARD(k)    (!UI_KEY_IS_SHAPE(k) && (k) != UI_KEY_WAIT && ((k) & UI_KEY_GUARD_BIT) != 0)
#define UI_KEY_GUARD_MAX_TEXT 0xfffu

// Fills out with LVGL key codes (characters as themselves); returns how many, or UI_KEYS_ERR with *err_pos set to the
// offset of the first thing not understood (or of the token that did not fit in max).
size_t ui_keys_parse(char const *seq, uint32_t *out, size_t max, size_t *err_pos);
// Whether focus (what "fw ui" reports as the focus) passes guard, a value ui_keys_parse made from seq
bool ui_keys_guard_ok(uint32_t guard, char const *seq, char const *focus);
