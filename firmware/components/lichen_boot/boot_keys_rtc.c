// "fw boot keys" (#379), the badge side: the one-shot flag in LP RAM. #330 measured it: RTC_NOINIT survives a software
// restart, a panic and the interrupt watchdog, and is lost at a power-off and the CH340 EN reset, which is what a
// one-shot bench flag wants (no NVS wear, nothing left armed after a power cycle).
#include "boot_keys.h"

#include "esp_attr.h"
#include "fw_console.h"

#include <string.h>

static RTC_NOINIT_ATTR boot_keys_flag_t s_flag;

boot_keys_t boot_keys_take_armed(void) {
    return boot_keys_take(&s_flag);
}

void boot_keys_cmd(char const *args) {
    if (!console_dev_gate("boot keys")) {
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (*args == '\0') {
        console_reply("boot keys: %s armed for the next boot", boot_keys_name(boot_keys_peek(&s_flag)));
        return;
    }
    boot_keys_t k;
    if (!boot_keys_parse(args, &k)) {
        console_reply("boot keys <down|esc+bksp|none>: unknown combo, nothing armed");
        return;
    }
    boot_keys_arm(&s_flag, k);
    if (k == BOOT_KEYS_NONE) {
        console_reply("boot keys: none armed");
    } else {
        console_reply("boot keys: %s held at the next boot (once, a software restart only; fw reboot)",
                      boot_keys_name(k));
    }
}
