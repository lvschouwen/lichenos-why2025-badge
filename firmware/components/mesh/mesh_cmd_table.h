#pragma once
// MeshCore's console command tables (#208): the top-level commands it registers and the "fw mesh" subcommands, as
// data with their matching rule, so the dispatch is one loop and a host test pins the names (host_tests/
// mesh_cmd_table_test.c). Pure; the handlers are mesh_cmd*.cpp's, in the same order (mesh_cmd.cpp checks it).
//
// The per-app command prefix (#229) goes where mesh_console_init registers MESH_CONSOLE_CMDS. The on-air allow list's
// check (#289) does not belong in this dispatch, which sees only a word and raw text: it sits in the senders
// (mesh_onair.h); only its own command, "onair", is in the table.

#include "fw_console.h" // console_sub_t (#464): a peer header mesh may include (tools/layers.conf)

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The top-level console commands, in the order they are registered
typedef enum {
    MESH_CON_MESH,
    MESH_CON_CONTACTS,
    MESH_CON_MSGS,
    MESH_CON_CHANNELS,
    MESH_CON_CHANNEL,
    MESH_CON_ADVERT,
    MESH_CON_ONAIR, // #289: the console's on-air allow list
    MESH_CON_COUNT,
} mesh_con_id_t;

typedef struct {
    char const          *name;
    char const          *usage; // what "fw help" lists
    console_sub_t const *rules; // the sub-command rules it registers with (#464), NULL for none
} mesh_con_t;

extern mesh_con_t const MESH_CONSOLE_CMDS[MESH_CON_COUNT];

// How a subcommand's word matches the line after "mesh ", and what its handler gets: console_sub.h's match rules (#464)
typedef console_match_t mesh_sub_match_t;
#define MESH_SUB_EXACT    CON_MATCH_EXACT
#define MESH_SUB_PREFIX   CON_MATCH_PREFIX
#define MESH_SUB_WORD     CON_MATCH_WORD
#define MESH_SUB_WORD_RAW CON_MATCH_WORD_RAW

// The "fw mesh" subcommands in the order they are tried: those that answer whether MeshCore runs or not first, then
// those that need it running
typedef enum {
    MESH_SUB_START,
    MESH_SUB_STOP,
    MESH_SUB_AUTOSTART,
    MESH_SUB_HIST,
    MESH_SUB_NOTIFY,
    MESH_SUB_ORDER,
    MESH_SUB_NAME,
    MESH_SUB_RADIO,
    MESH_SUB_PATHHASH,
    MESH_SUB_PATH,
    MESH_SUB_SEND,
    MESH_SUB_DM,
    MESH_SUB_OUT,
    MESH_SUB_TRACE,
    MESH_SUB_DISCOVER,
    MESH_SUB_TELEMETRY,
    MESH_SUB_HISTORY,
    MESH_SUB_REMOTE,
    MESH_SUB_LINK,
    MESH_SUB_CHATS,
    MESH_SUB_IDENT_SIMULATE, // #455: a test arm that blocks TX until restart; its row has no word in the release image
    MESH_SUB_IDENT,          // any other "ident ...": only the developer-mode gate (#464), the handler answers usage
    MESH_SUB_ADVERT,
    MESH_SUB_LOC,
    MESH_SUB_AUTOADD,
    MESH_SUB_SCOPE,
    MESH_SUB_AIRLOG,
    MESH_SUB_CHAT, // #438: a debug command (developer mode)
    MESH_SUB_COUNT, // none matched
} mesh_sub_id_t;

// A "fw mesh" row is a console_sub_t (#464): run_command checks its gate and origin before cmd_mesh runs, and the
// owner's bit says MeshCore must run ("MeshCore is not running" and the like otherwise)
typedef console_sub_t mesh_sub_t;
#define MESH_SUB_RUNNING CON_F_OWNER

// One row per id, then CONSOLE_SUB_END: "fw mesh" registers with it as its sub-command rules
extern mesh_sub_t const MESH_SUBS[MESH_SUB_COUNT + 1];

// "fw onair" (#289) and "fw advert": the rules they register with
extern console_sub_t const MESH_ONAIR_RULES[];
extern console_sub_t const MESH_ADVERT_RULES[];

// What sub's handler gets of args (the line after "mesh "), or NULL when it does not match
char const *mesh_sub_match(mesh_sub_t const *sub, char const *args);

// Whether id (from mesh_sub_find) needs MeshCore running: true for MESH_SUB_COUNT, so an unknown line and the status
// ("fw mesh" alone) say why not while it is stopped
bool mesh_sub_needs_running(mesh_sub_id_t id);

// The first subcommand that matches args, in table order, with what its handler gets in *rest; MESH_SUB_COUNT (and
// *rest NULL) for none
mesh_sub_id_t mesh_sub_find(char const *args, char const **rest);

#ifdef __cplusplus
}
#endif
