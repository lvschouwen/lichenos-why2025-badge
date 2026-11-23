#pragma once

// What the OS keeps per registered app (#520): one zeroed PSRAM block made when the app registers and freed when it
// leaves, reached from the app's table slot (app_ctx.h, lichen_sys_app_state in lichen_sys_app.h). It replaces the fixed
// per-slot arrays each module used to size by the most apps, so the number of apps costs no internal RAM and no module
// bounds it. Each field has one owner module and its lock; PSRAM is never touched inside a critical section (#271), so
// those locks are mutexes or the fields are atomics.
//
// Pure (no ESP-IDF): the layout is shared by the modules below and by the host tests.

#include "app_life.h"
#include "mem_book.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_STATE_DIR_CAP 40 // "/storage/<id>" and "/storage/lichen/apps/<id>" with the longest id (lichen_fs.c asserts)
#define APP_STATE_SD_CAP  48

typedef struct {
    app_life_t life;         // lichen_app_life.c, under its mutex (zeroed = REGISTERED)
    mem_use_t  mem;          // the memory book's row (lichen_mem.c, under the book's mutex); mem.ctx 0 until first charged
    uint32_t   caps_refused; // calls refused for a missing capability, atomic (app_caps.c)
    uint32_t   badge;        // the tile badge's count (lichen_badge.c, under its mutex)
    uint8_t    timer_count;  // booked LVGL timers, atomic (lichen_ui_cb.c)
    bool       timer_dead;   // the app stopped: its timers delete themselves, atomic
    bool       notify_badged; // a badge notify_ui.c set (LVGL task)
    bool       dir_made;     // lichen_fs: dir_path was made (under its mutex)
    uint32_t   sd_generation; // lichen_fs: the SD mount sd_path was made in; 0 = not yet
    char       dir_path[APP_STATE_DIR_CAP];
    char       sd_path[APP_STATE_SD_CAP];
} app_state_t;
