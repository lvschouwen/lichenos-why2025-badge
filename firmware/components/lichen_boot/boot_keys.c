#include "boot_keys.h"

#include "factory_plan.h"

#include <string.h>

#define CODE_DOWN    0x34 // TCA8418 code of the Down arrow (board/tca8418_keymap.h; boot_keys_test checks it)
#define DOWN_HOLD_MS 2000 // #128 step 20: "hold the Down arrow for about 2 seconds"
#define HOLD_SPARE_MS 500 // past the combo's own time: its 100 ms tick and the poll's pause never cut it short

static char const *const names[BOOT_KEYS_COUNT] = {"none", "down", "esc+bksp"};

bool boot_keys_parse(char const *word, boot_keys_t *out) {
    if (!word) {
        return false;
    }
    for (int k = 0; k < BOOT_KEYS_COUNT; k++) {
        if (strcmp(word, names[k]) == 0) {
            *out = (boot_keys_t)k;
            return true;
        }
    }
    return false;
}

char const *boot_keys_name(boot_keys_t k) {
    return (unsigned)k < BOOT_KEYS_COUNT ? names[k] : "none";
}

void boot_keys_arm(boot_keys_flag_t *f, boot_keys_t k) {
    if (k == BOOT_KEYS_NONE || (unsigned)k >= BOOT_KEYS_COUNT) {
        *f = (boot_keys_flag_t){0};
        return;
    }
    *f = (boot_keys_flag_t){.magic = BOOT_KEYS_MAGIC, .combo = (uint32_t)k, .check = ~(BOOT_KEYS_MAGIC ^ (uint32_t)k)};
}

boot_keys_t boot_keys_peek(boot_keys_flag_t const *f) {
    bool valid = f->magic == BOOT_KEYS_MAGIC && f->check == ~(f->magic ^ f->combo) && f->combo > BOOT_KEYS_NONE &&
                 f->combo < BOOT_KEYS_COUNT;
    return valid ? (boot_keys_t)f->combo : BOOT_KEYS_NONE;
}

boot_keys_t boot_keys_take(boot_keys_flag_t *f) {
    boot_keys_flag_t held = *f;
    *f                    = (boot_keys_flag_t){0};
    return boot_keys_peek(&held);
}

boot_keys_plan_t boot_keys_plan(boot_keys_t k) {
    switch (k) {
    case BOOT_KEYS_DOWN:
        return (boot_keys_plan_t){.count = 1, .codes = {CODE_DOWN}, .hold_ms = DOWN_HOLD_MS};
    case BOOT_KEYS_ESC_BKSP:
        return (boot_keys_plan_t){.count   = 2,
                                  .codes   = {FACTORY_COMBO_KEY_A, FACTORY_COMBO_KEY_B},
                                  .hold_ms = FACTORY_COMBO_HOLD_MS + HOLD_SPARE_MS};
    default:
        return (boot_keys_plan_t){0};
    }
}
