#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

// A Lichen app (#66): MeshCore, Settings, About. Everything with an lv_obj_t runs in LVGL's context (core 1);
// start_background runs from main.c on core 0. Any member except name may be NULL.
// tile_text is called every second from the LVGL task for each app on home: it must be quick (copy a cached string under
// the app's own lock, never wait on the mesh or the network).
typedef struct lichen_app {
    char const *name;                                                  // "MeshCore"
    void (*draw_icon)(lv_obj_t *parent, int size_px);                  // 32 and 64 px
    bool (*tile_text)(char *line1, size_t c1, char *line2, size_t c2); // false = nothing new
    void (*open)(lv_obj_t *screen);                                    // build its screens; runs in LVGL context
    // Settings > <name>: the page is a menu.h menu titled with the app's name. settings adds its rows; about (after them)
    // shows what the app is and runs on (version, ids). The entry is listed when either is set.
    void (*settings)(lv_obj_t *page);
    bool (*start_background)(void);                                    // from main.c, core 0
    // A firmware update is being received (#76): stop everything of the app's that is not needed for that (tasks, radios,
    // scans) after saving what must survive. Called once from the OTA task, may block for seconds; the badge restarts
    // afterwards, so there is no start again. Not called for a file push.
    void (*stop_for_update)(void);
    void (*about)(lv_obj_t *page); // see settings; LVGL context
} lichen_app_t;

#define LICHEN_MAX_APPS 8

// Before shell_home(). The app struct must stay valid for as long as the badge runs (a static const).
void lichen_register_app(lichen_app_t const *app);
size_t lichen_app_count(void);
lichen_app_t const *lichen_app_at(size_t index);
