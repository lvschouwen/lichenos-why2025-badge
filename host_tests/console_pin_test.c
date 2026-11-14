// Host test (#464): the console's gate and origin pin. For every registered command and every sub-command with a rule,
// what the console accepts and refuses per developer mode (on, off), build (developer, release) and origin (the USB
// console, Wi-Fi, the Console app on the badge). Built twice: console_pin_dev_test (CONFIG_LICHEN_DEV_COMMANDS, checks
// the "on" and "off" columns) and console_pin_test (a release-like build, checks the "rel" column).
//
// The expectations below are the pin: they were written against the code before #464 moved the rules into tables, and
// a change to one of them changes what the badge accepts. The evaluator under them is what the console decides today
// (pin_decide): the dev gate and origin layer that runs before a command's own code.
#include "console_os_rules.h"
#include "console_sub.h"
#include "dev_gate.h"
#include "dev_mode.h"
#include "sd_rules.h"
#include "mesh_onair.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checked;

// One outcome per origin, in the order USB console, Wi-Fi, Console app:
//   R  the command's own code runs (it may still answer usage)
//   D  refused: developer mode is off, or not in this build
//   O  refused: the origin (USB console only)
//   H  refused: the USB hold is on (#289) and it is not the USB console
//   ?  unknown command
typedef struct {
    char const *line;
    char const *on;  // developer build, developer mode on
    char const *off; // developer build, developer mode off
    char const *rel; // release build (developer mode is always off there)
    bool        held; // the USB hold is on (#289)
} pin_t;

// clang-format off
static pin_t const PINS[] = {
    // fw_console's own
    {"help",                       "RRR", "RRR", "RRR", false},
    {"help all",                   "RRR", "RRR", "RRR", false},
    {"mem",                        "RRR", "RRR", "RRR", false},
    {"mem blocks psram",           "RRR", "RRR", "RRR", false},
    {"mem window",                 "RRR", "DDD", "RRR", false}, // release: "not in this build" from fw_console itself
    {"mem window reset",           "RRR", "DDD", "RRR", false},
    {"assets",                     "RRR", "RRR", "RRR", false},
    {"slot",                       "RRR", "RRR", "RRR", false},
    {"slot other",                 "RRR", "RRR", "RRR", false},
    {"slot other",                 "RHH", "RHH", "RHH", true},
    {"slot bogus",                 "RRR", "RRR", "RRR", true},
    {"reboot",                     "RRR", "RRR", "RRR", false},
    {"dev",                        "RRR", "RRR", "???", false},
    {"dev status",                 "RRR", "RRR", "???", false},
    {"dev on",                     "ROO", "ROO", "???", false},
    {"dev off",                    "RRR", "RRR", "???", false},
    {"dev bogus",                  "RRR", "RRR", "???", false},
    {"nosuchcommand",              "???", "???", "???", false},
    // the GATED[] table of dev_gate.c: open forms exactly, every other form needs developer mode
    {"noscreen",                   "RRR", "DDD", "???", false},
    {"thermal",                    "RRR", "RRR", "RRR", false},
    {"thermal test warn",          "RRR", "DDD", "DDD", false},
    {"thermal test shutdown YES",  "RRR", "DDD", "DDD", false},
    {"thermal seen reset",         "RRR", "DDD", "DDD", false},
    {"thermal bogus",              "RRR", "DDD", "DDD", false},
    {"psram",                      "RRR", "RRR", "RRR", false},
    {"psram test 64",              "RRR", "DDD", "DDD", false},
    {"psram speed 80 yes",         "RRR", "DDD", "DDD", false},
    {"psram dlboot 3 clear",       "RRR", "DDD", "DDD", false},
    {"stress",                     "RRR", "DDD", "DDD", false},
    {"stress 5",                   "RRR", "DDD", "DDD", false},
    {"stress status",              "RRR", "RRR", "RRR", false},
    {"stress stop",                "RRR", "RRR", "RRR", false},
    {"idle",                       "RRR", "RRR", "RRR", false},
    {"idle test 30",               "RRR", "DDD", "DDD", false},
    {"setup",                      "RRR", "DDD", "???", false},
    {"setup reset",                "RRR", "DDD", "???", false},
    {"sensors",                    "RRR", "RRR", "RRR", false},
    {"sensors raw",                "RRR", "RRR", "RRR", false},
    {"sensors boot",               "RRR", "RRR", "RRR", false},
    {"sensors start",              "RRR", "DDD", "DDD", false},
    {"sensors boot on",            "RRR", "DDD", "DDD", false},
    {"notify",                     "RRR", "RRR", "RRR", false},
    {"notify off",                 "RRR", "RRR", "RRR", false},
    {"notify test dm",             "RRR", "DDD", "DDD", false},
    {"notify testing",             "RRR", "RRR", "RRR", false},
    {"vib",                        "RRR", "RRR", "RRR", false},
    {"vib 50",                     "RRR", "RRR", "RRR", false},
    {"vib test",                   "RRR", "DDD", "DDD", false},
    // MeshCore's commands (app meshcore)
    {"mesh",                       "RRR", "RRR", "RRR", false},
    {"mesh start",                 "RRR", "RRR", "RRR", false},
    {"mesh send #badgetest hi",    "RRR", "RRR", "RRR", false},
    {"mesh dm 0123456789ab hi",    "RRR", "RRR", "RRR", false},
    {"mesh advert flood",          "RRR", "RRR", "RRR", false},
    {"mesh radio eu_narrow",       "RRR", "RRR", "RRR", false},
    {"mesh chat open #badgetest",  "RRR", "DDD", "DDD", false},
    {"mesh chat close",            "RRR", "DDD", "DDD", false},
    {"mesh chats",                 "RRR", "RRR", "RRR", false},
    {"mesh ident simulate",        "RRR", "DDD", "DDD", false},
    {"mesh ident",                 "RRR", "DDD", "DDD", false},
    {"mesh airlog",                "RRR", "DDD", "RRR", false}, // release: the handler is compiled out
    {"mesh airlog 20",             "RRR", "DDD", "RRR", false},
    {"meshcore mesh chat close",   "RRR", "DDD", "DDD", false},
    {"meshcore mesh airlog",       "RRR", "DDD", "RRR", false},
    {"meshcore sd test",           "???", "???", "???", false}, // the wrong owner under a prefix
    {"advert",                     "RRR", "RRR", "RRR", false},
    {"advert flood",               "RRR", "RRR", "RRR", false},
    {"contacts",                   "RRR", "RRR", "RRR", false},
    {"msgs 5",                     "RRR", "RRR", "RRR", false},
    {"channels",                   "RRR", "RRR", "RRR", false},
    {"channel add #badgetest",     "RRR", "RRR", "RRR", false},
    {"onair",                      "RRR", "RRR", "RRR", false},
    {"onair on",                   "RRR", "RRR", "RRR", false},
    {"onair off",                  "ROO", "ROO", "ROO", false},
    {"onair clear",                "ROO", "ROO", "ROO", false},
    {"onair allow #badgetest",     "ROO", "ROO", "ROO", false},
    {"onair remove #badgetest",    "ROO", "ROO", "ROO", false},
    {"onair allow",                "RRR", "RRR", "RRR", false}, // usage: a target is missing
    {"onair off now",              "RRR", "RRR", "RRR", false}, // usage
    {"onair bogus",                "RRR", "RRR", "RRR", false}, // usage
    {"meshcore onair off",         "ROO", "ROO", "ROO", false},
    // handler gates (console_dev_gate inside the command)
    {"boot",                       "RRR", "RRR", "RRR", false},
    {"boot keys",                  "RRR", "DDD", "DDD", false},
    {"boot keys down",             "RRR", "DDD", "DDD", false},
    {"boot fail display",          "RRR", "DDD", "RRR", false}, // release: the arm is compiled out, the status prints
    {"crash",                      "RRR", "RRR", "RRR", false},
    {"crash dump",                 "RRR", "RRR", "RRR", false},
    {"crash clear",                "RRR", "RRR", "RRR", false},
    {"crash test panic",           "RRR", "DDD", "RRR", false}, // release: usage
    {"crash test wdt",             "RRR", "DDD", "RRR", false},
    {"crash test",                 "RRR", "RRR", "RRR", false}, // usage
    {"sd",                         "RRR", "RRR", "RRR", false},
    {"sd status",                  "RRR", "RRR", "RRR", false},
    {"sd mount",                   "RRR", "RRR", "RRR", false},
    {"sd test",                    "RRR", "DDD", "RRR", false}, // release: usage
    {"sd test now",                "RRR", "RRR", "RRR", false}, // usage
    {"sd cat /sd/notes.txt",       "RRR", "DDD", "RRR", false},
    {"sd cat",                     "RRR", "DDD", "RRR", false},
    {"sd format",                  "ROO", "ROO", "ROO", false},
    {"sd format YES",              "ROO", "ROO", "ROO", false},
    {"sd formatx",                 "RRR", "RRR", "RRR", false}, // usage
    {"app",                        "RRR", "RRR", "RRR", false},
    {"app ctx",                    "RRR", "RRR", "RRR", false},
    {"app background",             "RRR", "RRR", "RRR", false},
    {"app crash meshcore",         "RRR", "DDD", "RRR", false}, // release: usage
    {"app crash meshcore cb",      "RRR", "DDD", "RRR", false},
    // handler origin rules
    {"token",                      "RRR", "RRR", "RRR", false},
    {"token new",                  "ROO", "ROO", "ROO", false},
    {"token bogus",                "RRR", "RRR", "RRR", false},
    {"factory-reset",              "RRR", "RRR", "RRR", false},
    {"factory-reset plan",         "RRR", "RRR", "RRR", false},
    {"factory-reset plan all",     "RRR", "RRR", "RRR", false},
    {"factory-reset settings YES", "ROO", "ROO", "ROO", false},
    {"factory-reset all YES",      "ROO", "ROO", "ROO", false},
    {"factory-reset all YES",      "ROO", "ROO", "ROO", true},
    {"factory-reset settings",     "RRR", "RRR", "RRR", false}, // "add YES"
    {"factory-reset bogus",        "RRR", "RRR", "RRR", false}, // usage
    {"nvs-enc",                    "RRR", "RRR", "RRR", false},
    {"nvs-enc burn",               "RRR", "RRR", "RRR", false}, // the warning; release: no burn arm, the state
    {"nvs-enc burn YES",           "ROO", "ROO", "RRR", false},
    {"nvs-down",                   "ROO", "ROO", "???", false},
    {"nvs-down YES",               "ROO", "ROO", "???", false},
    {"nvs-slots",                  "RRR", "RRR", "RRR", false},
    // registered with console_register_dev: not in a release image
    {"ui",                         "RRR", "DDD", "???", false},
    {"ui answer no",               "RRR", "DDD", "???", false},
    {"keys send enter",            "RRR", "DDD", "???", false},
    {"battery",                    "RRR", "DDD", "???", false},
    {"trial nowifi",               "RRR", "DDD", "???", false},
    {"soak",                       "RRR", "DDD", "???", false},
    // no rule today: transmitting or restarting forms open from everywhere (#464 lists them for the owner)
    {"c6",                         "RRR", "RRR", "RRR", false},
    {"c6 reset",                   "RRR", "RRR", "RRR", false},
    {"c6 reset rom",               "RRR", "RRR", "RRR", false},
    {"c6 reattach",                "RRR", "RRR", "RRR", false},
    {"c6 verify",                  "RRR", "RRR", "RRR", false},
    {"c6 reflash",                 "RRR", "RRR", "RRR", false},
    {"lora",                       "RRR", "RRR", "RRR", false},
    {"lora on",                    "RRR", "RRR", "RRR", false},
    {"lora off",                   "RRR", "RRR", "RRR", false},
    {"lora log on",                "RRR", "RRR", "RRR", false},
    {"airplane off",               "RRR", "RRR", "RRR", false},
    {"poweroff",                   "RRR", "RRR", "RRR", false},
    {"led blink",                  "RRR", "RRR", "RRR", false},
    {"wifi scan",                  "RRR", "RRR", "RRR", false},
    // the other commands: no rule
    {"net",                        "RRR", "RRR", "RRR", false},
    {"clock sync",                 "RRR", "RRR", "RRR", false},
    {"home",                       "RRR", "RRR", "RRR", false},
    {"uicheck",                    "RRR", "RRR", "RRR", false},
    {"screensaver",                "RRR", "RRR", "RRR", false},
    {"lock now",                   "RRR", "RRR", "RRR", false},
    {"whatsnew reset",             "RRR", "RRR", "RRR", false},
    {"bench",                      "RRR", "RRR", "RRR", false},
    {"flashlight",                 "RRR", "RRR", "RRR", false},
    {"i2c scan",                   "RRR", "RRR", "RRR", false},
    {"ota off",                    "RRR", "RRR", "RRR", false},
    {"c6log 500",                  "RRR", "RRR", "RRR", false},
    {"storage",                    "RRR", "RRR", "RRR", false},
    {"tasks",                      "RRR", "RRR", "RRR", false},
    {"bl 40 20",                   "RRR", "RRR", "RRR", false},
    {"get bl.",                    "RRR", "RRR", "RRR", false},
    {"set bl.display 40",          "RRR", "RRR", "RRR", false},
    {"disp",                       "RRR", "RRR", "RRR", false},
    {"fonts",                      "RRR", "RRR", "RRR", false},
    {"qr hello",                   "RRR", "RRR", "RRR", false},
    {"qrcode qr hello",            "RRR", "RRR", "RRR", false},
};
// clang-format on

// ---- the console's command list (registration, per build) ------------------------------------------------------

typedef enum {
    REG_BOTH,     // console_register in every build
    REG_DEVBUILD, // console_register under CONFIG_LICHEN_DEV_COMMANDS (a release image has no such command)
    REG_DEV,      // console_register_dev: developer mode gates the whole command, a release image has none
    REG_KCONFIG,  // under its own Kconfig symbol (off by default); the developer test builds it in
} reg_t;

typedef struct {
    char const *name;
    reg_t       reg;
    int         app; // 0: the OS's; else 1 + the index in IDS
} cmd_t;

static char const *const IDS[] = {"meshcore", "qrcode"};

static cmd_t const CMDS[] = {
    {"help", REG_BOTH, 0},        {"mem", REG_BOTH, 0},          {"assets", REG_BOTH, 0},
    {"slot", REG_BOTH, 0},        {"reboot", REG_BOTH, 0},       {"dev", REG_DEVBUILD, 0},
    {"noscreen", REG_DEVBUILD, 0}, {"thermal", REG_BOTH, 0},     {"psram", REG_BOTH, 0},
    {"stress", REG_BOTH, 0},      {"idle", REG_BOTH, 0},         {"setup", REG_DEVBUILD, 0},
    {"sensors", REG_BOTH, 0},     {"notify", REG_BOTH, 0},       {"vib", REG_BOTH, 0},
    {"mesh", REG_BOTH, 1},        {"contacts", REG_BOTH, 1},     {"msgs", REG_BOTH, 1},
    {"channels", REG_BOTH, 1},    {"channel", REG_BOTH, 1},      {"advert", REG_BOTH, 1},
    {"onair", REG_BOTH, 1},       {"qr", REG_BOTH, 2},           {"boot", REG_BOTH, 0},
    {"crash", REG_BOTH, 0},       {"sd", REG_BOTH, 0},           {"app", REG_BOTH, 0},
    {"token", REG_BOTH, 0},       {"factory-reset", REG_BOTH, 0}, {"nvs-enc", REG_BOTH, 0},
    {"nvs-down", REG_KCONFIG, 0}, {"nvs-slots", REG_BOTH, 0},    {"ui", REG_DEV, 0},
    {"keys", REG_DEV, 0},         {"battery", REG_DEV, 0},       {"trial", REG_DEV, 0},
    {"soak", REG_DEV, 0},         {"c6", REG_BOTH, 0},           {"lora", REG_BOTH, 0},
    {"airplane", REG_BOTH, 0},    {"poweroff", REG_BOTH, 0},     {"led", REG_BOTH, 0},
    {"wifi", REG_BOTH, 0},        {"net", REG_BOTH, 0},          {"clock", REG_BOTH, 0},
    {"home", REG_BOTH, 0},        {"uicheck", REG_BOTH, 0},      {"screensaver", REG_BOTH, 0},
    {"lock", REG_BOTH, 0},        {"whatsnew", REG_BOTH, 0},     {"bench", REG_BOTH, 0},
    {"flashlight", REG_BOTH, 0},  {"i2c", REG_BOTH, 0},          {"ota", REG_BOTH, 0},
    {"c6log", REG_BOTH, 0},       {"storage", REG_BOTH, 0},      {"tasks", REG_BOTH, 0},
    {"bl", REG_BOTH, 0},          {"get", REG_BOTH, 0},          {"set", REG_BOTH, 0},
    {"disp", REG_BOTH, 0},        {"fonts", REG_BOTH, 0},
};

#ifdef CONFIG_LICHEN_DEV_COMMANDS
#define DEV_BUILD true
#else
#define DEV_BUILD false
#endif

static cmd_t const *cmd_find(char const *name, int app) {
    for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        if (strcmp(CMDS[i].name, name) != 0) {
            continue;
        }
        if (app != 0 && CMDS[i].app != app) {
            return NULL;
        }
        bool built = CMDS[i].reg == REG_BOTH || DEV_BUILD;
        return built ? &CMDS[i] : NULL;
    }
    return NULL;
}

// ---- what the commands check themselves before #464 (handler gates and origin rules) ---------------------------

static bool word_is(char const *args, char const *word) {
    size_t n = strlen(word);
    return strncmp(args, word, n) == 0 && (args[n] == '\0' || args[n] == ' ');
}

// console_dev_gate calls inside handlers: boot_keys_rtc.c, boot_progress.c, crash.c, sd_console.c, safe_mode.c,
// mem_window_watch.c, mesh_cmd_send.cpp (airlog, chat). True when the form reaches one.
static bool handler_dev_gate(char const *name, char const *args) {
    if (strcmp(name, "boot") == 0) {
        return word_is(args, "keys") || (DEV_BUILD && strncmp(args, "fail ", 5) == 0);
    }
    if (strcmp(name, "crash") == 0) {
        return DEV_BUILD && (strcmp(args, "test panic") == 0 || strcmp(args, "test wdt") == 0);
    }
    if (strcmp(name, "app") == 0) {
        return DEV_BUILD && strncmp(args, "crash ", 6) == 0 && args[6];
    }
    if (strcmp(name, "mem") == 0) {
        return DEV_BUILD && word_is(args, "window"); // a release image binds no window: fw_console answers
    }
    if (strcmp(name, "mesh") == 0) {
        return DEV_BUILD && (word_is(args, "airlog") || word_is(args, "chat"));
    }
    return false;
}

// console_from_uart / console_origin checks inside handlers: true when the form is refused from origin
static bool handler_origin_refuses(char const *name, char const *args, console_origin_t origin) {
    bool uart = origin == CONSOLE_ORIGIN_UART;
    if (strcmp(name, "token") == 0) {
        return strcmp(args, "new") == 0 && !uart;
    }
    if (strcmp(name, "nvs-down") == 0) {
        return !uart;
    }
    if (strcmp(name, "nvs-enc") == 0) {
        return DEV_BUILD && strcmp(args, "burn YES") == 0 && !uart; // the burn arm: its own Kconfig, in the dev test
    }
    if (strcmp(name, "onair") == 0) {
        char const *rest;
        return !onair_op_permitted(onair_op_parse(args, &rest), origin);
    }
    if (strcmp(name, "factory-reset") == 0) {
        char word[16] = "", yes[8] = "";
        sscanf(args, "%15s %7s", word, yes);
        bool level = strcmp(word, "settings") == 0 || strcmp(word, "all") == 0;
        return level && strcmp(yes, "YES") == 0 && console_erase_refuses(origin);
    }
    return false;
}

// ---- the sub-command tables the commands register (#464) ---------------------------------------------------------

typedef struct {
    char const          *name;
    console_sub_t const *rules;
} rules_of_t;

static rules_of_t const RULES[] = {
    {"slot", OS_SLOT_RULES},
    {"reboot", OS_REBOOT_RULES},
    {"dev", OS_DEV_RULES},
    {"sd", SD_RULES},
};

static console_sub_t const *rules_of(char const *name) {
    for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++) {
        if (RULES[i].name && strcmp(RULES[i].name, name) == 0) {
            return RULES[i].rules;
        }
    }
    return NULL;
}

// ---- the decision -----------------------------------------------------------------------------------------------

static char pin_decide(char const *text, bool dev_on, console_origin_t origin, bool held) {
    char        buf[128];
    char const *args;
    int         app;
    snprintf(buf, sizeof(buf), "%s", text);
    char        *name = dev_gate_split(buf, IDS, 2, &app, &args);
    cmd_t const *cmd  = cmd_find(name, app);
    if (!cmd) {
        return '?';
    }
    if (cmd->reg == REG_DEV && !dev_on) {
        return 'D';
    }
    if (dev_gate_needs_dev(name, args) && !dev_on) {
        return 'D'; // run_command's gate (dev_gate.c)
    }
    console_ask_t ask = {.dev_on = dev_on, .origin = origin, .held = held};
    switch (console_sub_rule(console_sub_find(rules_of(name), args), &ask)) { // run_command's rules_allow
        case CON_RULE_DEV: return 'D';
        case CON_RULE_ORIGIN: return 'O';
        case CON_RULE_HOLD: return 'H';
        case CON_RULE_RUN: break;
    }
    if (handler_dev_gate(name, args) && !dev_on) {
        return 'D';
    }
    if (handler_origin_refuses(name, args, origin)) {
        return 'O';
    }
    return 'R';
}

static console_origin_t const ORIGINS[3] = {CONSOLE_ORIGIN_UART, CONSOLE_ORIGIN_WIFI, CONSOLE_ORIGIN_LOCAL};

static void check_column(pin_t const *p, char const *col, char const *what, bool dev_on) {
    for (int o = 0; o < 3; o++) {
        char got = pin_decide(p->line, dev_on, ORIGINS[o], p->held);
        checked++;
        if (got != col[o]) {
            printf("FAIL \"%s\"%s (%s, origin %s): got %c, pinned %c\n", p->line, p->held ? " held" : "", what,
                   o == 0 ? "usb" : o == 1 ? "wifi" : "app", got, col[o]);
            failures++;
        }
    }
}

// Every pinned line names a command of the list, and every command of the list is pinned at least once
static void check_coverage(void) {
    for (size_t c = 0; c < sizeof(CMDS) / sizeof(CMDS[0]); c++) {
        bool seen = false;
        for (size_t i = 0; i < sizeof(PINS) / sizeof(PINS[0]) && !seen; i++) {
            size_t n = strlen(CMDS[c].name);
            seen     = strncmp(PINS[i].line, CMDS[c].name, n) == 0 && (PINS[i].line[n] == ' ' || !PINS[i].line[n]);
        }
        if (!seen) {
            printf("FAIL command %s has no pinned line\n", CMDS[c].name);
            failures++;
        }
    }
}

int main(void) {
    for (size_t i = 0; i < sizeof(PINS) / sizeof(PINS[0]); i++) {
        if (DEV_BUILD) {
            check_column(&PINS[i], PINS[i].on, "developer build, mode on", true);
            check_column(&PINS[i], PINS[i].off, "developer build, mode off", false);
        } else {
            check_column(&PINS[i], PINS[i].rel, "release build", false);
        }
    }
    check_coverage();
    printf("console_pin%s: %d checks, %d failures\n", DEV_BUILD ? " (developer build)" : " (release)", checked,
           failures);
    return failures ? 1 : 0;
}
