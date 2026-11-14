// MeshCore's console command tables (#208): mesh_cmd_table.h
#include "mesh_cmd_table.h"

#include <string.h>

mesh_con_t const MESH_CONSOLE_CMDS[MESH_CON_COUNT] = {
    [MESH_CON_MESH]     = {"mesh",
                           "mesh | mesh start | mesh stop | mesh autostart [on|off] | mesh name <name> | mesh radio <preset | freq bw sf cr> [txdbm] | "
                           "mesh link | mesh out | mesh chats | mesh hist [compact|scan [n] [bad|bad1] [int] [from<offset>]] | mesh send <channel> <text> | "
                           "mesh dm <12 hex> <text> | mesh notify [test dm|chan <name>|fail|mention] | mesh order [reset] | mesh advert ... | mesh loc ... | mesh autoadd ...",
                           MESH_SUBS},
    [MESH_CON_CONTACTS] = {"contacts", "contacts (nodes heard, newest first) | contacts export [<12 hex>] | contacts import "
                                       "<part> | end | cancel", NULL},
    [MESH_CON_MSGS]     = {"msgs", "msgs [n] (last n messages)", NULL},
    [MESH_CON_CHANNELS] = {"channels", "channels (the channels the badge can read)", NULL},
    [MESH_CON_CHANNEL]  = {"channel", "channel add <name> <base64 key> | channel add #name | channel rm <index|name> | "
                                      "channel show <index|name> | channel rename <index|name> <new name>", NULL},
    [MESH_CON_ADVERT]   = {"advert", "advert [zero|flood] (send an advert now; default as the Advertise setting)",
                           MESH_ADVERT_RULES},
    [MESH_CON_ONAIR]    = {"onair", "onair [on | off | allow <channel|key digits> | remove <channel|key digits> | clear] "
                                    "(the console's on-air allow list; off and changes only from the USB console)", MESH_ONAIR_RULES},
};

// The rows' flags: R needs MeshCore running; TX transmits (CON_F_TX): the on-air allow list and the bench scope judge
// those sends in the senders (mesh_onair.h, #289/#290), not here, so their origin stays ANY (the build check's allow
// list, tools/check_console_rules.py)
#define R  MESH_SUB_RUNNING
#define TX CON_F_TX
#define OPEN CON_GATE_OPEN, CON_ORIGIN_ANY
#define DEV  CON_GATE_DEV, CON_ORIGIN_ANY

mesh_sub_t const MESH_SUBS[MESH_SUB_COUNT + 1] = {
    // whether MeshCore runs or not
    [MESH_SUB_START]     = {"start", MESH_SUB_EXACT, OPEN, TX}, // MeshCore claims the radio and adverts
    [MESH_SUB_STOP]      = {"stop", MESH_SUB_EXACT, OPEN, 0},
    [MESH_SUB_AUTOSTART] = {"autostart", MESH_SUB_WORD, OPEN, 0}, // #115
    [MESH_SUB_HIST]      = {"hist", MESH_SUB_WORD, OPEN, 0},      // the journal's own status
    [MESH_SUB_NOTIFY]    = {"notify", MESH_SUB_WORD, OPEN, 0},    // #50
    [MESH_SUB_ORDER]     = {"order", MESH_SUB_WORD_RAW, OPEN, 0}, // #189
    // MeshCore running
    [MESH_SUB_NAME]           = {"name ", MESH_SUB_PREFIX, OPEN, R},
    [MESH_SUB_RADIO]          = {"radio", MESH_SUB_WORD_RAW, OPEN, R},
    [MESH_SUB_PATHHASH]       = {"pathhash", MESH_SUB_WORD_RAW, OPEN, R},
    [MESH_SUB_PATH]           = {"path ", MESH_SUB_PREFIX, OPEN, R},
    [MESH_SUB_SEND]           = {"send ", MESH_SUB_PREFIX, OPEN, R | TX},
    [MESH_SUB_DM]             = {"dm ", MESH_SUB_PREFIX, OPEN, R | TX},
    [MESH_SUB_OUT]            = {"out", MESH_SUB_EXACT, OPEN, R},
    [MESH_SUB_TRACE]          = {"trace", MESH_SUB_WORD, OPEN, R | TX},
    [MESH_SUB_DISCOVER]       = {"discover ", MESH_SUB_PREFIX, OPEN, R | TX},
    [MESH_SUB_TELEMETRY]      = {"telemetry", MESH_SUB_WORD, OPEN, R | TX},
    [MESH_SUB_HISTORY]        = {"history", MESH_SUB_WORD, OPEN, R | TX}, // "<who> on" schedules polls
    [MESH_SUB_REMOTE]         = {"remote", MESH_SUB_WORD, OPEN, R | TX},
    [MESH_SUB_LINK]           = {"link", MESH_SUB_EXACT, OPEN, R},
    [MESH_SUB_CHATS]          = {"chats", MESH_SUB_EXACT, OPEN, R},
    // #455: a test arm that blocks TX until restart (developer image only); the release row has no word, so the string
    // is not in the image and nothing can match it. The id stays so the handler table keeps one row per id.
    [MESH_SUB_IDENT_SIMULATE] = {CON_DEV_WORD("ident simulate"), MESH_SUB_EXACT, DEV, R},
    // every other "ident" form takes the same gate (a release answers "not in this build"), the handler the usage
    [MESH_SUB_IDENT]          = {"ident", MESH_SUB_WORD, DEV, R},
    [MESH_SUB_ADVERT]         = {"advert", MESH_SUB_WORD, OPEN, R | TX},
    [MESH_SUB_LOC]            = {"loc", MESH_SUB_WORD, OPEN, R},
    [MESH_SUB_AUTOADD]        = {"autoadd", MESH_SUB_WORD, OPEN, R},
    [MESH_SUB_SCOPE]          = {"scope", MESH_SUB_WORD, OPEN, R}, // #290: the console scope
    // #408: a debug command (developer mode); the release image has no handler arm, so no word either
    [MESH_SUB_AIRLOG]         = {CON_DEV_WORD("airlog"), MESH_SUB_WORD, DEV, R},
    [MESH_SUB_CHAT]           = {"chat", MESH_SUB_WORD, DEV, R}, // #438: opens a screen from the console
    [MESH_SUB_COUNT]          = CONSOLE_SUB_END,
};

// "onair" (#289): switching the list off and editing it weaken the on-air guard, so the USB console only. Fail closed:
// the status and "on" (which only narrows) stay open, every other form takes the rule.
console_sub_t const MESH_ONAIR_RULES[] = {
    {"", MESH_SUB_EXACT, OPEN, 0},
    {"on", MESH_SUB_EXACT, OPEN, 0},
    {NULL, CON_MATCH_REST, CON_GATE_OPEN, CON_ORIGIN_UART, 0},
    CONSOLE_SUB_END,
};

// "advert [zero|flood]" sends an advert now; the on-air guard judges it in the sender (advert_tx_locked)
console_sub_t const MESH_ADVERT_RULES[] = {
    {NULL, CON_MATCH_REST, OPEN, TX},
    CONSOLE_SUB_END,
};

#undef R
#undef TX
#undef OPEN
#undef DEV

char const *mesh_sub_match(mesh_sub_t const *sub, char const *args) {
    return console_sub_match(sub, args);
}

bool mesh_sub_needs_running(mesh_sub_id_t id) {
    return id >= MESH_SUB_COUNT || (MESH_SUBS[id].flags & MESH_SUB_RUNNING);
}

mesh_sub_id_t mesh_sub_find(char const *args, char const **rest) {
    for (int i = 0; i < MESH_SUB_COUNT; i++) {
        char const *r = mesh_sub_match(&MESH_SUBS[i], args);
        if (r) {
            *rest = r;
            return (mesh_sub_id_t)i;
        }
    }
    *rest = NULL;
    return MESH_SUB_COUNT;
}
