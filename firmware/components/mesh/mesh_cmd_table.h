#pragma once
// MeshCore's console command tables (#208): the top-level commands it registers and the "fw mesh" subcommands, as
// data with their matching rule, so the dispatch is one loop and a host test pins the names (host_tests/
// mesh_cmd_table_test.c). Pure; the handlers are mesh_cmd*.cpp's, in the same order (mesh_cmd.cpp checks it).
//
// The per-app command prefix (#229) goes where mesh_console_init registers MESH_CONSOLE_CMDS. The on-air allow-list
// (#289) does not belong in this dispatch, which sees only a word and raw text: see mesh_sub_run in mesh_cmd.cpp.

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
    MESH_CON_COUNT,
} mesh_con_id_t;

typedef struct {
    char const *name;
    char const *usage; // what "fw help" lists
} mesh_con_t;

extern mesh_con_t const MESH_CONSOLE_CMDS[MESH_CON_COUNT];

// How a subcommand's word matches the line after "mesh ", and what its handler gets:
// EXACT: the whole line, nothing; PREFIX: the word with its space ("send "), what follows; WORD: the word alone or
// with a space, what follows that space; WORD_RAW: as WORD, with the space (handlers that skip it themselves).
typedef enum { MESH_SUB_EXACT, MESH_SUB_PREFIX, MESH_SUB_WORD, MESH_SUB_WORD_RAW } mesh_sub_match_t;

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
    MESH_SUB_IDENT_SIMULATE,
    MESH_SUB_ADVERT,
    MESH_SUB_LOC,
    MESH_SUB_AUTOADD,
    MESH_SUB_SCOPE,
    MESH_SUB_AIRLOG,
    MESH_SUB_COUNT, // none matched
} mesh_sub_id_t;

typedef struct {
    char const      *word;
    mesh_sub_match_t match;
    bool             needs_running; // MeshCore must run: "MeshCore is not running" (and the like) otherwise
} mesh_sub_t;

extern mesh_sub_t const MESH_SUBS[MESH_SUB_COUNT];

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
