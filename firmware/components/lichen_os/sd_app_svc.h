#pragma once
// The SD apps' loader service (#523), its parts' shared view: sd_app_loader.c (task, load, unload, trampolines) and
// sd_app_scan.c (the mount scan). Not for anyone else.

#include "lichen_app.h"
#include "lichen_manifest.h"
#include "lichen_sd_app.h"
#include "sd_app_rules.h"

#include "esp_elf.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_APPS_MAX     12   // apps shown from one card; more are left out with a log line
#define SD_APP_MSG_LEN  96   // a refusal's text

// One SD app the OS knows: made by the scan, in PSRAM, freed after lichen_unregister_app's done ran. The registered
// lichen_app_t is inside it and its strings point into m, so the struct stays whole until then.
typedef struct sd_app_rec {
    struct sd_app_rec *next;
    lichen_manifest_t  m;
    lichen_app_t       app;
    lichen_ctx_t       ctx;
    sd_app_t           sda;      // under sd_app_lock()
    esp_elf_t          elf;      // valid while image
    bool               image;    // an ELF image is held
    bool               hooks_set;
    bool               stop_called;   // the stop hook was called for this load (an update's stop, or the unload's)
    int                stop_inflight; // callers of the stop hook right now: the image is freed only after they left
    bool               gone;     // lichen_unregister_app was called
    lichen_sd_hooks_t  hooks;    // a copy, valid while hooks_set; called only while sd_app_hooks_live
    void              *screen;   // lv_obj_t: the screen waiting for the hooks' open; LVGL task only
    char               msg[SD_APP_MSG_LEN]; // what the waiting screen shows after a refusal
} sd_app_rec_t;

// The one mutex of the service's state (a leaf: nothing is called while it is held, never the LVGL lock)
void sd_app_lock(void);
void sd_app_unlock(void);
// The app of ctx, NULL when none (stale, the OS's, not an SD app's). The record stays until its unregister is done.
sd_app_rec_t *sd_app_find(lichen_ctx_t ctx);
// Adds a made, registered record to the list.
void sd_app_link(sd_app_rec_t *rec);

// The scan (loader task): registers the tile of every app that passes, refuses and notifies the others.
void sd_app_scan(void);
// The trampolines of lichen_app_t (sd_app_loader.c)
void sd_app_tramp_open(lv_obj_t *screen);
bool sd_app_tramp_stop(void);

#ifdef __cplusplus
}
#endif
