#pragma once

#include <stdbool.h>
#include <stdint.h>

// "fw boot keys" (#379): a key combo pretended held at the next boot, once, so the remote bench can reach the paths a
// hand on the keyboard reaches (the #66 reset question, the #128 boot screen view). The flag is a word in LP RAM
// (RTC_NOINIT): it survives a software restart and is lost with the power (and the CH340 EN reset, #330), and it costs
// no NVS write. The parse, the flag's encoding and what each combo presses are pure (boot_keys.c, host_tests/
// boot_keys_test.c); boot_keys_rtc.c keeps the flag and adds the subcommand, main plays it.

typedef enum { BOOT_KEYS_NONE, BOOT_KEYS_DOWN, BOOT_KEYS_ESC_BKSP, BOOT_KEYS_COUNT } boot_keys_t;

// Power-on RAM is noise: a magic word plus a check word make a false hit unlikely
#define BOOT_KEYS_MAGIC 0x4C424B59u // "LBKY"
typedef struct {
    uint32_t magic;
    uint32_t combo;
    uint32_t check; // ~(magic ^ combo)
} boot_keys_flag_t;

// "down", "esc+bksp" or "none"; false for anything else (out untouched)
bool        boot_keys_parse(char const *word, boot_keys_t *out);
char const *boot_keys_name(boot_keys_t k);
// Arms k for the next boot; BOOT_KEYS_NONE (or an unknown value) clears the flag
void        boot_keys_arm(boot_keys_flag_t *f, boot_keys_t k);
// What the flag holds, BOOT_KEYS_NONE unless it is valid; leaves it as it is
boot_keys_t boot_keys_peek(boot_keys_flag_t const *f);
// One shot: clears the flag first (a crash while the keys are held must not hold them again on every boot), then
// returns what it held, BOOT_KEYS_NONE unless it was valid
boot_keys_t boot_keys_take(boot_keys_flag_t *f);

#define BOOT_KEYS_MAX 2
typedef struct {
    uint8_t  count;
    uint8_t  codes[BOOT_KEYS_MAX]; // TCA8418 codes, pressed in this order, released together
    uint32_t hold_ms;              // from the last press to the release
} boot_keys_plan_t;

// The keys of k and how long they are held: each path's real time with a margin. Esc + Backspace: the #66 combo's
// FACTORY_COMBO_HOLD_MS (its question arms on the hold, the release does not cancel it); Down: the #128 "hold about
// 2 s" that must switch the boot screen's view once, not on every repeat. count 0 for BOOT_KEYS_NONE.
boot_keys_plan_t boot_keys_plan(boot_keys_t k);

// The badge side (boot_keys_rtc.c). The armed combo, cleared: main.c's start_keyboard, once, after keyboard_start.
boot_keys_t boot_keys_take_armed(void);
// "fw boot keys [<down|esc+bksp|none>]" (args: the text after "keys"); dev-gated. Console task.
void        boot_keys_cmd(char const *args);
