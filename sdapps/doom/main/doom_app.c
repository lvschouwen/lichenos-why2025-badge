// DOOM as an SD-card app (#525): main() checks for the WAD, hands the OS its hooks and starts the engine on a task of its
// own; stop ends that task and frees everything the engine still holds. The picture, the keys and the loading bar
// are doom_ui.c's (#526). GPL-3.0-or-later (the engine is GPL-2.0-or-later, sha1.c GPL-3.0-or-later).
#include "lichen_app.h"
#include "lichen_notify.h"
#include "lichen_sd_app.h"
#include "lichen_task.h"
#include "theme.h"

#include "doom_app.h"
#include "doom_engine.h"
#include "doom_ui.h"

#include <stdio.h>
#include <stdlib.h>

#define WAD_NAME    "freedoom1.wad"
#define ZONE_MIB    "6"
#define STACK_BYTES (16 * 1024)
#define STOP_WAIT   3000 // ms the engine task gets to see the stop flag (it checks once per tick)
#define STOP_TOTAL  4500 // the loader gives stop 5 s: the engine's wait and the LVGL task's detach share this

typedef enum { START_NO_WAD, START_FAILED, START_OK } start_t;

static volatile bool stop_req;
static volatile bool task_done;
static volatile bool loading; // doomgeneric_Create runs: the WAD load, which never looks at stop_req
static start_t       start_state = START_NO_WAD;

static void engine_task(void *arg) {
    (void)arg;
    // -iwad: the file in the app's folder (the file shims' relative path); -mb: the zone, in MiB (psram_kib of the manifest)
    char *argv[] = {(char *)"doom", (char *)"-iwad", (char *)WAD_NAME, (char *)"-mb", (char *)ZONE_MIB, NULL};
    loading = true;
    doomgeneric_Create(5, argv);
    loading = false;
    while (!stop_req) {
        doomgeneric_Tick();
        vTaskDelay(1); // the engine paces itself with DG_SleepMs; this yields in a frame that waits for nothing
    }
    // closed by the user, not by the engine: its exit functions save the config and stop its subsystems (the engine is no
    // longer ticking, and the file calls take the SD lock per call)
    doom_engine_shutdown();
    task_done = true; // keep last: stop frees the engine's blocks as soon as it sees this
}

void doom_engine_ending(void) {
    task_done = true; // the last thing the task does before exit(), which never returns
}

bool doom_load_cancelled(void) {
    return loading && stop_req;
}

// The WAD's size in bytes, 0 when it is not there (or empty: the engine would only say so)
static uint32_t wad_size(void) {
    FILE *f = fopen(WAD_NAME, "rb");
    if (!f) {
        return 0;
    }
    long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : 0;
    fclose(f);
    return size > 0 ? (uint32_t)size : 0;
}

// a text screen below the status bar (R7), for what is not the game: no WAD, no memory
static void message_screen(lv_obj_t *screen, char const *text) {
    lv_obj_set_style_pad_hor(screen, THEME_PAD, 0);
    lv_obj_set_style_pad_top(screen, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, text);
}

static void open_screen(lv_obj_t *screen) {
    if (start_state == START_OK) {
        if (!doom_ui_open(screen)) {
            // the engine is running but has nowhere to draw; the screen says so and the user closes it
            return;
        }
    } else if (start_state == START_FAILED) {
        message_screen(screen, "DOOM could not start (not enough memory). Close it and open it again.");
    } else {
        // R11: what is missing and how to get it
        message_screen(screen, "No game files yet. Put " WAD_NAME " in /lichen/apps/doom on the SD card and open DOOM again.");
    }
}

// The engine task is gone when it said so (task_done) or when it left through exit(): the OS's book then has no task of
// this app left (lichen_task_leftovers does not wait with 0).
static bool engine_gone(void) {
    return task_done || lichen_task_leftovers(lichen_ctx_self(), 0) == 0;
}

static bool doom_stop(void) {
    stop_req = true;
    TickType_t begin = xTaskGetTickCount();
    bool started = start_state == START_OK;
    for (int waited = 0; started && !engine_gone() && waited < STOP_WAIT; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (started && !engine_gone()) {
        return false; // the engine task is stuck: the loader reports it, nothing is freed under a running task
    }
    uint32_t spent = (uint32_t)(xTaskGetTickCount() - begin) * portTICK_PERIOD_MS;
    if (!doom_ui_detach(spent < STOP_TOTAL ? STOP_TOTAL - spent : 200)) {
        return false; // the LVGL task still has the picture's memory: nothing is freed under it
    }
    char summary[128];
    doom_ui_summary(summary, sizeof(summary));
    if (summary[0]) {
        lichen_notify(lichen_ctx_self(), LN_INFO, "DOOM", summary, 0); // the fps average of the run (#526)
    }
    doom_free_all();
    return true;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    // the task first: open runs on the LVGL task as soon as the hooks are in and reads engine_started
    uint32_t size = wad_size();
    doom_ui_set_wad_size(size);
    if (size) {
        start_state = lichen_task_start(lichen_ctx_self(), "doom", engine_task, NULL, STACK_BYTES, LICHEN_PRIO_BACKGROUND, NULL)
                          ? START_OK
                          : START_FAILED;
    }
    lichen_sd_hooks_t const hooks = {.size = sizeof(hooks), .open = open_screen, .stop = doom_stop};
    if (!lichen_sd_app_hooks(&hooks)) {
        doom_stop();
        return 1;
    }
    return 0;
}
