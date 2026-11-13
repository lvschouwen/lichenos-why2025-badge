// MeshCore's console command tables (#208): mesh_cmd_table.h
#include "mesh_cmd_table.h"

#include <string.h>

mesh_con_t const MESH_CONSOLE_CMDS[MESH_CON_COUNT] = {
    [MESH_CON_MESH]     = {"mesh",
                           "mesh | mesh start | mesh stop | mesh autostart [on|off] | mesh name <name> | mesh radio <preset | freq bw sf cr> [txdbm] | "
                           "mesh link | mesh out | mesh chats | mesh ident simulate | mesh hist [compact|scan [n] [bad|bad1] [int] [from<offset>]] | mesh send <channel> <text> | "
                           "mesh dm <12 hex> <text> | mesh notify [test dm|chan <name>|fail|mention] | mesh order [reset] | mesh advert ... | mesh loc ... | mesh autoadd ..."},
    [MESH_CON_CONTACTS] = {"contacts", "contacts (nodes heard, newest first) | contacts export [<12 hex>] | contacts import "
                                       "<part> | end | cancel"},
    [MESH_CON_MSGS]     = {"msgs", "msgs [n] (last n messages)"},
    [MESH_CON_CHANNELS] = {"channels", "channels (the channels the badge can read)"},
    [MESH_CON_CHANNEL]  = {"channel", "channel add <name> <base64 key> | channel add #name | channel rm <index|name> | "
                                      "channel show <index|name> | channel rename <index|name> <new name>"},
    [MESH_CON_ADVERT]   = {"advert", "advert [zero|flood] (send an advert now; default as the Advertise setting)"},
};

mesh_sub_t const MESH_SUBS[MESH_SUB_COUNT] = {
    // whether MeshCore runs or not
    [MESH_SUB_START]     = {"start", MESH_SUB_EXACT, false},
    [MESH_SUB_STOP]      = {"stop", MESH_SUB_EXACT, false},
    [MESH_SUB_AUTOSTART] = {"autostart", MESH_SUB_WORD, false}, // #115
    [MESH_SUB_HIST]      = {"hist", MESH_SUB_WORD, false},      // the journal's own status
    [MESH_SUB_NOTIFY]    = {"notify", MESH_SUB_WORD, false},    // #50
    [MESH_SUB_ORDER]     = {"order", MESH_SUB_WORD_RAW, false}, // #189
    // MeshCore running
    [MESH_SUB_NAME]           = {"name ", MESH_SUB_PREFIX, true},
    [MESH_SUB_RADIO]          = {"radio", MESH_SUB_WORD_RAW, true},
    [MESH_SUB_PATHHASH]       = {"pathhash", MESH_SUB_WORD_RAW, true},
    [MESH_SUB_PATH]           = {"path ", MESH_SUB_PREFIX, true},
    [MESH_SUB_SEND]           = {"send ", MESH_SUB_PREFIX, true},
    [MESH_SUB_DM]             = {"dm ", MESH_SUB_PREFIX, true},
    [MESH_SUB_OUT]            = {"out", MESH_SUB_EXACT, true},
    [MESH_SUB_TRACE]          = {"trace", MESH_SUB_WORD, true},
    [MESH_SUB_DISCOVER]       = {"discover ", MESH_SUB_PREFIX, true},
    [MESH_SUB_TELEMETRY]      = {"telemetry", MESH_SUB_WORD, true},
    [MESH_SUB_HISTORY]        = {"history", MESH_SUB_WORD, true},
    [MESH_SUB_REMOTE]         = {"remote", MESH_SUB_WORD, true},
    [MESH_SUB_LINK]           = {"link", MESH_SUB_EXACT, true},
    [MESH_SUB_CHATS]          = {"chats", MESH_SUB_EXACT, true},
    [MESH_SUB_IDENT_SIMULATE] = {"ident simulate", MESH_SUB_EXACT, true},
    [MESH_SUB_ADVERT]         = {"advert", MESH_SUB_WORD, true},
    [MESH_SUB_LOC]            = {"loc", MESH_SUB_WORD, true},
    [MESH_SUB_AUTOADD]        = {"autoadd", MESH_SUB_WORD, true},
    [MESH_SUB_SCOPE]          = {"scope", MESH_SUB_WORD, true}, // #290: the console scope
};

char const *mesh_sub_match(mesh_sub_t const *sub, char const *args) {
    size_t len = strlen(sub->word);
    if (sub->match == MESH_SUB_EXACT) {
        return strcmp(args, sub->word) == 0 ? args + len : NULL;
    }
    if (strncmp(args, sub->word, len) != 0) {
        return NULL;
    }
    if (sub->match == MESH_SUB_PREFIX) {
        return args + len;
    }
    if (args[len] != ' ' && args[len]) {
        return NULL; // another word that starts the same ("history" is not "hist")
    }
    return args + len + (sub->match == MESH_SUB_WORD && args[len] ? 1 : 0);
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
