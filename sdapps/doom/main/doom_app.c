// DOOM as an SD-card app (#525): main() checks for the WAD, hands the OS its hooks and starts the engine on a task of its
// own; stop ends that task and frees everything the engine still holds. The skeleton reaches the title loop: video and keys
// are #526. GPL-3.0-or-later (the engine is GPL-2.0-or-later, sha1.c GPL-3.0-or-later).
#include "lichen_app.h"
#include "lichen_sd_app.h"
#include "lichen_task.h"
#include "theme.h"

#include "doom_app.h"
#include "doom_engine.h"

#include <stdio.h>
#include <stdlib.h>

#define WAD_NAME    "freedoom1.wad"
#define ZONE_MIB    "6"
#define STACK_BYTES (16 * 1024)
#define STOP_WAIT   4000 // ms the engine task gets to see the stop flag (it checks once per tick)

static volatile bool stop_req;
static volatile bool task_done;
static bool          engine_started;

static void engine_task(void *arg) {
    (void)arg;
    // -iwad: the file in the app's folder (the file shims' relative path); -mb: the zone, in MiB (psram_kib of the manifest)
    char *argv[] = {(char *)"doom", (char *)"-iwad", (char *)WAD_NAME, (char *)"-mb", (char *)ZONE_MIB, NULL};
    doomgeneric_Create(5, argv);
    while (!stop_req) {
        doomgeneric_Tick();
        vTaskDelay(1); // the engine paces itself with DG_SleepMs; this yields in a frame that waits for nothing
    }
    task_done = true;
}

static bool wad_present(void) {
    FILE *f = fopen(WAD_NAME, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

static void open_screen(lv_obj_t *screen) {
    lv_obj_set_style_pad_all(screen, THEME_PAD, 0);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    if (engine_started) {
        lv_label_set_text(label, "DOOM is loading. The picture and the keys come in a later update.");
    } else {
        // R11: what is missing and how to get it
        lv_label_set_text(label, "No game files yet. Put " WAD_NAME " in /lichen/apps/doom on the SD card and open DOOM again.");
    }
}

static bool doom_stop(void) {
    stop_req = true;
    for (int waited = 0; engine_started && !task_done && waited < STOP_WAIT; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (engine_started && !task_done) {
        return false; // the engine task is stuck: the loader reports it, nothing is freed under a running task
    }
    // the task may still be returning (done is its last write), the blocks are no longer touched
    doom_free_all();
    return true;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    // the task first: open runs on the LVGL task as soon as the hooks are in and reads engine_started
    if (wad_present()) {
        engine_started = lichen_task_start(lichen_ctx_self(), "doom", engine_task, NULL, STACK_BYTES, LICHEN_PRIO_BACKGROUND, NULL);
    }
    lichen_sd_hooks_t const hooks = {.size = sizeof(hooks), .open = open_screen, .stop = doom_stop};
    if (!lichen_sd_app_hooks(&hooks)) {
        doom_stop();
        return 1;
    }
    return 0;
}
