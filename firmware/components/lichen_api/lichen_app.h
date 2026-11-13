#pragma once

#include "lvgl.h"
#include "settings_backup.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A Lichen app (#66): MeshCore, Settings, About. Everything with an lv_obj_t runs in LVGL's context (core 1);
// start_background runs from main.c on core 0. Any member except name may be NULL.
// tile_text is called every second from the LVGL task for each app on home: it must be quick (copy a cached string under
// the app's own lock, never wait on the mesh or the network).
typedef struct lichen_app {
    char const *name;                                                  // "MeshCore"
    // "meshcore": 1..13 of a-z 0-9 _ (app_ns.h). Names the app's NVS namespace (lichen_settings.h) and its console
    // forms ("fw setup reset <id>"); the OS wipes an app by it (#66). Required.
    char const *id;
    void (*draw_icon)(lv_obj_t *parent, int size_px);                  // 32 and 64 px
    bool (*tile_text)(char *line1, size_t c1, char *line2, size_t c2); // false = nothing new
    void (*open)(lv_obj_t *screen);                                    // build its screens; runs in LVGL context
    // Settings > <name>: the page is a menu.h menu titled with the app's name. settings adds its rows; about (after them)
    // shows what the app is and runs on (version, ids). The entry is listed when either is set.
    void (*settings)(lv_obj_t *page);
    bool (*start_background)(void);                                    // from main.c, core 0
    // A firmware update is being received (#76): stop everything of the app's that is not needed for that (tasks, radios,
    // scans) after saving what must survive. Called once from the OTA task, may block for seconds; the badge restarts
    // afterwards, so there is no start again. Not called for a file push. True when the app stopped (and saved): the
    // OS stops the LoRa radio only when every app said so (#84 T7).
    bool (*stop_for_update)(void);
    void (*about)(lv_obj_t *page); // see settings; LVGL context
    // The app's first-run questions, if it has any: done=false asks them again at the next open, done=true marks
    // them answered ("fw setup reset|done <id>"). The app keeps the flag in its own settings.
    void (*first_run_set)(bool done);
    // The user opened one of the app's notifications (lichen_notify.h, #62) in the centre or with the cloud key: show
    // that place (target as the app passed it, "" for none). LVGL context; the entry is already marked read. NULL:
    // the shell opens the app as from home.
    void (*open_notification)(char const *target, uint32_t id);
    // The Messages key (▲, #79): the shell has just opened the app from home (its open() ran); open the app's message
    // list on top of it, unless open() showed something else first (first-run questions). LVGL context. The first
    // registered app that sets it gets the key; NULL: not a messaging app.
    void (*open_messages)(void);
    // What the app sends to the notification service is configurable (#50): the page for it (shell_open, LVGL context).
    // Settings > Notifications lists a row for each app that has one, below the per-app On/Off switches.
    void (*notify_settings_open)(void);
    // What the OS knows of the app by name (#230 E1d); all optional, so the OS carries no app's literals.
    // alias: a short console form besides id ("mesh" for "meshcore": "fw setup done mesh").
    char const *alias;
    // Keys of the app's namespace (a.<id>) that a factory reset keeps, NULL-terminated: state about the badge's airtime
    // rather than a choice (MeshCore's last advert, so the advert interval holds and a reset loop never floods).
    char const *const *keep_keys;
    // The directory name under /storage the app keeps its files in: protected in Files; storage_secret hides it
    // completely (identity and keys). NULL: none.
    char const *storage_root;
    bool        storage_secret;
    // The app's settings that may go in the SD backup (#169), each with its own namespace; allow-list as in settings_backup.h
    sb_key_t const *backup_keys;
    size_t          backup_key_count;
    // The app's secrets class (#288, lichen_settings.h app_secret_*): every key of its namespace that starts with this
    // prefix ("pw" for MeshCore's repeater passwords), 1..8 of a-z 0-9 _; NULL: the app has no secrets. A prefix that
    // breaks those rules, or a backup or keep key under it, is refused at registration (the app, its backup group,
    // the key). Set by name like every member, so later members never move it.
    char const *secret_prefix;
} lichen_app_t;

// Size of the app table. Home scrolls to the selection, so this only bounds the arrays; lichen_register_app logs an error
// when it is full.
#define LICHEN_MAX_APPS 24 // #173: 16 was full with the 2026-11-03 apps; ~110 B internal .bss per slot; ids allow 32 (SCREEN_APP_MAX)

// An app's context (#230): 8 bits of slot and 24 bits of generation, not a pointer. lichen_register_app hands it out;
// the gate calls take it (#230 E1b-E1e) and refuse one that is stale or made up (logged, counted in "fw app ctx").
// 0 is never valid.
typedef uint32_t lichen_ctx_t;
#define LICHEN_CTX_NONE ((lichen_ctx_t)0)

// Before shell_home(). The app struct must stay valid for as long as the badge runs (a static const). Returns the
// app's context, LICHEN_CTX_NONE when the table is full or the app's id or alias is refused (#340): already registered
// (as an id or an alias), one the OS keeps ("lichen", "test", the drive folders), no id or not an app id, or an alias
// equal to the app's own id. A refused app
// is skipped with an error in the log; boot goes on.
lichen_ctx_t lichen_register_app(lichen_app_t const *app);
// The context of the app the calling code runs for: inside its hooks (the shell enters it around every hook call and
// every lichen_ui_call it queued) and on its tasks (lichen_task_start; a task started from an app's task inherits it).
// The OS's context everywhere else: in an ISR, an esp_timer callback, an LVGL event or timer callback the app
// registered, a task the app did not start through lichen_task_start (docs/app-api.md "The app context"). Any task
// or ISR; never blocks.
lichen_ctx_t lichen_ctx_self(void);
// The table of registered apps is the OS's (lichen_sys_app.h, #340): an app knows only its own context.

#ifdef __cplusplus
}
#endif
