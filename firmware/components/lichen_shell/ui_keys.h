#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// `fw keys send <seq>` and its alias `fw keys press <seq>` (bench diagnostic): the key sequence tokenizer. Pure, host-tested in host_tests/ui_keys_test.c.
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
// The keys go through the real key path (#375): each one is pressed and released as a typed key would be, and the
// next goes down 250 ms after it. hold:<ms> after a key (phys in between is fine) keeps that key down ms long, at
// most UI_KEY_HOLD_MAX_MS (more is capped); the keys after it go down meanwhile, so two holds make a combo
// ("esc hold:1500 bksp hold:1300": both down for over a second). After text: put a '|' first ("text:ab|hold:500").
// The parser puts the hold value right after its key. phys anywhere in the sequence sends all its keys as physical
// ones: through the remote key lock's gate (#190), which then drops them as it drops the keyboard's.
#define UI_KEY_HOLD_BIT       0x10000000u
#define UI_KEY_HOLD_MAX_MS    10000u
#define UI_KEY_IS_HOLD(k)     (((k) & 0xf0000000u) == UI_KEY_HOLD_BIT)
#define UI_KEY_HOLD_MS(k)     ((k) & 0xffffu)
#define UI_KEY_PHYS           0x08000000u
// Whether k is a key to press, not a pause, a guard, needlock, phys or a hold
bool ui_keys_is_key(uint32_t k);
// ms (at least 1) from now to the stepper's next job: the next key at next_at while keys_left, else the end of the
// last gap or the first of the n_held holds' until times (ms ticks, wrap-safe) to run out. Once the keys are all down
// a gap already over no longer counts, so a hold after the last key waits for its own end instead of spinning.
uint32_t ui_keys_wait_ms(uint32_t now, uint32_t next_at, bool keys_left, uint32_t const *until, size_t n_held);

// Fills out with LVGL key codes (characters as themselves); returns how many, or UI_KEYS_ERR with *err_pos set to the
// offset of the first thing not understood (or of the token that did not fit in max).
size_t ui_keys_parse(char const *seq, uint32_t *out, size_t max, size_t *err_pos);
// Whether focus (what "fw ui" reports as the focus) passes guard, a value ui_keys_parse made from seq
bool ui_keys_guard_ok(uint32_t guard, char const *seq, char const *focus);
