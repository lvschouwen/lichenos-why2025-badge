#pragma once
// The SD apps' loader service (#523), its parts' shared view: sd_app_loader.c (task, load, unload, trampolines) and
// sd_app_scan.c (the mount scan). Not for anyone else.

#include "lichen_app.h"
#include "lichen_manifest.h"
#include "lichen_sd_app.h"
#include "sd_app_rules.h"

#include "esp_elf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
    SemaphoreHandle_t  stop_sem;    // the stop task gave it: its own, so a late stop of one app never wakes another's wait
    bool               stop_ok;     // what the stop hook answered (the stop task writes it before it gives stop_sem)
    bool               free_pending; // unregister is done but a screen still exists: the last delete event posts the free
    bool               stop_called;   // the stop hook was called for this load (an update's stop, or the unload's); the stop task is booked under the app, so the unload waits for it
    uint32_t           fault_ms; // lichen_sys_life_now_ms when a resumable unload gave up (sd_app_lock)
    bool               rescan_pending; // a manifest landed while the app ran (#553): reparsed when it is a tile again (sd_app_lock)
    bool               gone;     // lichen_unregister_app was called
    lichen_sd_hooks_t  hooks;    // a copy, valid while hooks_set; called only while sd_app_hooks_live
    void              *screen;   // lv_obj_t: the screen waiting for the hooks' open; LVGL task only
    void              *bar;      // lv_obj_t: the load bar and its text on the waiting screen; LVGL task only (sd_app_lock),
    void              *label;    // cleared by their own delete events
    sd_progress_t      prog;     // the loader task's throttle
    int                pct;      // the last percent handed to the bar (sd_app_lock)
    char               msg[SD_APP_MSG_LEN]; // what the waiting screen shows after a refusal
} sd_app_rec_t;

// The one mutex of the service's state (a leaf: nothing is called while it is held, never the LVGL lock)
void sd_app_lock(void);
void sd_app_unlock(void);
// The app of ctx, NULL when none (stale, the OS's, not an SD app's). The record stays until its unregister is done.
sd_app_rec_t *sd_app_find(lichen_ctx_t ctx);
// Whether an app of this id is still here, faulted or not (the scan leaves it alone).
bool sd_app_known(char const *id);
// Adds a made, registered record to the list.
void sd_app_link(sd_app_rec_t *rec);

// One app with a rescan marked that is a tile again (#553): its id into out, the mark cleared. False: none.
bool sd_app_rescan_due(char *out, size_t cap);
// The tile of id takes the caps and budgets of m (its name stays: the shell holds a pointer to it). False: a load began
// meanwhile, so it is marked for its unload instead.
bool sd_app_apply_manifest(char const *id, lichen_manifest_t const *m);

// The scan (loader task): registers the tile of every app that passes, refuses and notifies the others.
void sd_app_scan(void);
// Reads the manifest of the tile id again (#553), loader task
void sd_app_rescan(char const *id);
// The trampolines of lichen_app_t (sd_app_loader.c)
void sd_app_tramp_open(lv_obj_t *screen);
bool sd_app_tramp_stop(void);

#ifdef __cplusplus
}
#endif
