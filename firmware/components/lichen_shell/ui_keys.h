#pragma once

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

// Fills out with LVGL key codes (characters as themselves); returns how many, or UI_KEYS_ERR with *err_pos set to the
// offset of the first thing not understood (or of the token that did not fit in max).
size_t ui_keys_parse(char const *seq, uint32_t *out, size_t max, size_t *err_pos);
