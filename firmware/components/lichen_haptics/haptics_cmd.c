// "fw vib" (#68): the bench's way to feel the motor without raising notices
#include "fw_console.h"
#include "haptics.h"
#include "haptics_cmd.h"

#include <string.h>

#ifdef CONFIG_LICHEN_DEV_COMMANDS
#define VIB_USAGE "vib [on|off|test [tick|notify|alert]|strength low|medium|high]"
#else
#define VIB_USAGE "vib [on|off|strength low|medium|high]" // no test arm in a release image (#421)
#endif

static char const *const STRENGTH[] = {"low", "medium", "high"};

#ifdef CONFIG_LICHEN_DEV_COMMANDS // test arms are debug commands (#421): not in a release image
static bool test(char const *name) {
    if (!*name || strcmp(name, "notify") == 0) {
        haptics_force(HP_NOTIFY);
    } else if (strcmp(name, "tick") == 0) {
        haptics_force(HP_TICK);
    } else if (strcmp(name, "alert") == 0) {
        haptics_force(HP_ALERT);
    } else {
        return false;
    }
    return true;
}

#endif

static bool strength(char const *name) {
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, STRENGTH[i]) == 0) {
            haptics_set_strength((hp_strength_t)i);
            return true;
        }
    }
    return false;
}

static void cmd_vib(char const *args) {
    bool ok = true;
    if (strcmp(args, "on") == 0) {
        haptics_set_enabled(true);
    } else if (strcmp(args, "off") == 0) {
        haptics_set_enabled(false);
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    } else if (strncmp(args, "test", 4) == 0 && (args[4] == 0 || args[4] == ' ')) {
        ok = test(args[4] ? args + 5 : "");
#endif
    } else if (strncmp(args, "strength ", 9) == 0) {
        ok = strength(args + 9);
    } else if (*args) {
        ok = false;
    }
    if (!ok) {
        console_reply(VIB_USAGE);
        return;
    }
    console_reply("vib: GPIO%d, setting %s, strength %s%s", HAPTICS_GPIO, haptics_enabled() ? "on" : "off",
                  STRENGTH[haptics_strength()], haptics_playing() ? ", buzzing" : "");
}

void haptics_register_console(void) {
    console_register("vib", VIB_USAGE, cmd_vib);
}
