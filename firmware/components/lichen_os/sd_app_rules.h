#pragma once
// The SD apps' loader (#523), the pure part: what a mount scan says about an app, whether an open may load it and the
// load/unload state machine of one app. ESP-free, host-tested in host_tests/sd_app_rules_test.c; sd_app_loader.c is the
// glue that holds the locks and calls the ELF.
//
// One app is TILE (registered, no ELF loaded), LOADING (the loader task reads and relocates the ELF, runs main()),
// LOADED (its hooks are set and may be called), UNLOADING (stop, tasks, timers and queued UI calls, then the image is
// freed) or FAULTED (an unload missed its deadline: the image stays, nothing of it is ever called or freed again).
// The functions run under the loader's one mutex, a leaf: they only flip fields.

#include "lichen_manifest.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the scan ----------------------------------------------------------------------------------------------------

// The most stack an app's main() task may ask for in its manifest (KiB); 0 means the default
#define SD_APP_STACK_KIB_MAX 16

typedef enum {
    SD_APP_SCAN_OK,
    SD_APP_SCAN_BAD_MANIFEST, // the manifest does not parse (the parser's reason is in the text)
    SD_APP_SCAN_API,          // built for another API major, or a newer minor than this OS has
    SD_APP_SCAN_SHADOW,       // the id is a registered app's id or alias, or one the OS keeps
    SD_APP_SCAN_NO_ENTRY,     // the entry file next to the manifest is missing
    SD_APP_SCAN_STACK,        // stack_kib is above SD_APP_STACK_KIB_MAX
} sd_app_scan_t;

// The verdict for one directory of the apps folder: err is the parser's answer (m is valid only for OK), id_taken
// whether a registered app has that id or alias, entry_ok whether the entry file exists.
sd_app_scan_t sd_app_scan_verdict(lichen_manifest_err_t err, lichen_manifest_t const *m, bool id_taken, bool entry_ok);
// The plain reason for the person and the log, "needs app API 3.0, this is 2.1": into out (cap bytes, always
// terminated). dir_id names the app when the manifest could not.
void sd_app_scan_reason(sd_app_scan_t v, lichen_manifest_err_t err, lichen_manifest_t const *m, char const *dir_id,
                        char *out, size_t cap);

// ---- one app ------------------------------------------------------------------------------------------------------

typedef enum { SD_APP_TILE, SD_APP_LOADING, SD_APP_LOADED, SD_APP_UNLOADING, SD_APP_FAULTED } sd_app_state_t;

typedef struct {
    sd_app_state_t state;
    int            screens;  // the app's screens that exist (opened, not yet deleted by LVGL)
    bool           closing;  // the last screen was deleted: unload once nothing reopened it (cleared by an open)
    bool           removing; // the card went: nothing opens or loads from now on
} sd_app_t;

typedef enum {
    SD_OPEN_LOAD,    // TILE: the caller starts a load (the state is LOADING now)
    SD_OPEN_WAIT,    // LOADING: a load is under way, the screen waits for it
    SD_OPEN_NOW,     // LOADED: call the hooks' open
    SD_OPEN_HELD,    // the on-air hold is on (#289, #420): refused, hold_why says what holds
    SD_OPEN_BUSY,    // UNLOADING: still closing, try again in a moment
    SD_OPEN_FAULTED, // an earlier unload failed: the app stays closed until the card is out
    SD_OPEN_REMOVED, // the card is going
} sd_open_t;

// An open of the app's tile. hold_why: what holds now (console_usb_hold_why), NULL when nothing does; checked here for
// every open that would load, and again by the loader right before it reads the file.
sd_open_t sd_app_open(sd_app_t *a, char const *hold_why);
// A plain sentence for a refusal, for the screen, the notification and the log (hold_why is the tail of HELD's).
char const *sd_open_text(sd_open_t v);

typedef enum {
    SD_NEXT_NONE,   // nothing more (a failed load is back to TILE)
    SD_NEXT_OPEN,   // loaded: run the hooks' open
    SD_NEXT_UNLOAD, // loaded, but the screen already left or the card went: unload now
} sd_next_t;
// The loader finished: ok says the ELF is relocated, main() ran and the hooks are set.
sd_next_t sd_app_load_done(sd_app_t *a, bool ok);
// The load got as far as an image and then failed (no hooks, no task): LOADING goes straight to UNLOADING, so the unload's end
// (sd_app_unload_done) lands. The app's own "Loading" screen may still exist: it is the OS's, holds nothing of the ELF, and
// its delete event finds the tile. False when the app is not LOADING.
bool sd_app_load_abort(sd_app_t *a);
// The loader is about to read the file (#546): when the screen was closed meanwhile (closing, not a removal) there is nobody
// to open for, so the load is dropped: LOADING goes back to TILE and true is returned. Nothing was loaded, nothing to unload.
bool sd_app_load_skip(sd_app_t *a);
// One of the app's screens was deleted (LVGL's delete event, on the LVGL task). True when it was the last one and the app
// is LOADED, so the loader should plan the unload; a load under way remembers it and load_done answers UNLOAD. This is
// the only thing that lets an unload free the image: a screen that is still there may hold callbacks into it.
bool sd_app_screen_deleted(sd_app_t *a);
// LOADED and closing (no screen left) or removing becomes UNLOADING and returns true. False: it was opened again, or a
// screen still exists. A removal starts at once: it frees the image only after the shell closed the screens (do_free).
bool sd_app_unload_begin(sd_app_t *a);
// May the image and the record be freed (after lichen_unregister_app's done)? Only when LVGL deleted every screen of the app: a
// screen that is deleted later (an animation still running) delivers its delete event to the record and its ELF callbacks.
bool sd_app_may_free(sd_app_t const *a);
// May lichen_unregister_app be called for the app now? Only after a clean unload (or for a tile that never loaded), with
// no task of the app left: never for an app whose unload faulted, which keeps its image, tasks or timers, and tile.
bool sd_app_may_unregister(sd_app_t const *a, size_t tasks_left);
// The unload ended: clean frees the image (TILE, or gone with the card); not clean leaves FAULTED.
void sd_app_unload_done(sd_app_t *a, bool clean);
// The card went: from now on opens are refused. True when the app needs an unload (it is LOADED, or LOADING: the
// unload follows the load).
bool sd_app_remove(sd_app_t *a);
// May the hooks of the ELF be called now (LOADED, and not closing for good)?
bool sd_app_hooks_live(sd_app_t const *a);

// ---- exit (#533) --------------------------------------------------------------------------------------------------

// What the OS does for an app that called exit(), a bit set (SD_EXIT_NONE: nothing, the app is already leaving or not running)
#define SD_EXIT_NONE   0u
#define SD_EXIT_WAKE   1u // the loader may be waiting for hooks that will not come: wake it
#define SD_EXIT_HOME   2u // the app's screens are still up: close them (only its own, and only if it is on top), the delete events start the unload
#define SD_EXIT_UNLOAD 4u // no screen is left to start it: queue the unload now
unsigned sd_app_exit(sd_app_t *a);

// The context to enter around freeing the image: the app's while it is registered, none once lichen_unregister_app made it
// stale (#532: entering a stale context is refused and logged on the card-removal path).
bool sd_app_free_enters(bool unregistered);

// ---- the unload order (the invariant of #523) --------------------------------------------------------------------

typedef enum {
    SD_UNLOAD_STOP,  // 1: the app's stop hook
    SD_UNLOAD_TASKS, // 2: lichen_task_leftovers is empty
    SD_UNLOAD_UI,    // 3: booked timers dropped and queued UI calls drained
    SD_UNLOAD_FREE,  // 4: esp_elf_deinit
    SD_UNLOAD_DONE,  // the image is free
    SD_UNLOAD_FAULT, // a step failed: the image stays, never freed under what may still run
} sd_unload_step_t;
// The step after cur, given whether cur went well. Never skips a step and never reaches FREE after a failure.
sd_unload_step_t sd_unload_next(sd_unload_step_t cur, bool ok);

#ifdef __cplusplus
}
#endif
