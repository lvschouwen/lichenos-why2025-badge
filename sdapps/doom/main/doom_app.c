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
#include "doom_spike.h"
#include "doom_ui.h"

#include <stdio.h>
#include <stdlib.h>

#define WAD_NAME    "freedoom1.wad"
#define ZONE_MIB    "6"
#define STACK_BYTES (16 * 1024)
#define STOP_WAIT   3000 // ms the engine task gets to see the stop flag (it checks once per tick)
#define STOP_END_WAIT 500 // ms for the engine task to leave the OS's book after it said it was done
#define STOP_TOTAL  4500 // the loader gives stop 5 s: the engine's wait and the LVGL task's detach share this

// #558 spike: DOOM_SPIKE 1 = engine alone on core 0 (LICHEN_PRIO_BACKGROUND, as shipped), 2 = on core 1
// (LICHEN_PRIO_APP_LOOP, next to LVGL). Clock: the CPU's cycle counter (the machine-mode performance CSR the IDF's own
// esp_cpu_get_cycle_count reads; the export table has no microsecond clock), calibrated against lv_tick_get over 0.5 s
// (the port ticks LVGL every 5 ms) so no CPU frequency is assumed. If the counter does not run, the unit falls back to
// lv_tick_get ms (resolution 5 ms; the average from totals is still good, the max and the >33 counts are not).
// Only frames of a level (demos included), both before and after the Tick, count: a title screen is cheaper than the
// game, and the Tick that loads a level or runs the wipe is not a game frame.
// #558 measured the engine alone at 8.2 / 8.3 ms a frame on core 0 / core 1: no difference. Shipped on core 1 (APP_LOOP): LVGL
// draws nothing while the video surface is held, and core 0 carries the SDIO, console and mesh work (#527 plan). The canvas
// fallback shares core 1 with LVGL's render, which a measurement has still to show harmless.
#if DOOM_SPIKE == 1
#define ENGINE_PRIO LICHEN_PRIO_BACKGROUND
#else
#define ENGINE_PRIO LICHEN_PRIO_APP_LOOP
#endif

#if DOOM_SPIKE
extern int gamestate; // the engine's: GS_LEVEL is 0 (doomdef.h)
#define SPIKE_GS_LEVEL 0

static doom_spike_t spike; // the arithmetic is doom_spike.c's (host-tested); one clock for every timestamp
static bool         spike_cycles;
static uint32_t     spike_t_begin, spike_t_end; // lv_tick ms, the wall clock of the fps

static uint32_t now_tick(void) {
    return lv_tick_get();
}

static uint32_t now_cycles(void) {
    uint32_t c;
    __asm__ volatile("csrr %0, 0x7e2" : "=r"(c));
    return c;
}

// A tick edge, bounded: the LVGL port may be stalled
static uint32_t spike_tick_edge(void) {
    uint32_t t = lv_tick_get();
    for (int i = 0; i < 4000000 && lv_tick_get() == t; i++) {
    }
    return lv_tick_get();
}

// Before the clock is chosen the spike runs on ticks, so a sleep during the load has a clock to read
static void spike_calibrate(void) {
    doom_spike_init(&spike, now_tick, 1);
    uint32_t t0 = spike_tick_edge();
    uint32_t c0 = now_cycles();
    vTaskDelay(pdMS_TO_TICKS(500));
    uint32_t t1 = spike_tick_edge();
    uint32_t c1 = now_cycles();
    uint32_t ms = t1 - t0, dc = c1 - c0;
    uint32_t upm = ms && dc > ms * 1000u ? dc / ms : 0;
    spike_cycles = upm > 1;
    if (spike_cycles) {
        doom_spike_init(&spike, now_cycles, upm);
    }
}

void doom_spike_sleep_enter(void) {
    doom_spike_sleep_begin(&spike);
}

void doom_spike_sleep_leave(void) {
    doom_spike_sleep_end(&spike);
}

static void spike_summary(char *out, size_t cap) {
    doom_spike_format(&spike, out, cap, DOOM_SPIKE == 2 ? 1 : 0, spike_cycles, spike_t_end - spike_t_begin);
}
#endif

typedef enum { START_NO_WAD, START_FAILED, START_OK } start_t;

static volatile bool stop_req;
static volatile bool task_done;
static volatile bool loading; // doomgeneric_Create runs: the WAD load, which never looks at stop_req
static start_t       start_state = START_NO_WAD;

static void engine_task(void *arg) {
    (void)arg;
    // -iwad: the file in the app's folder (the file shims' relative path); -mb: the zone, in MiB (psram_kib of the manifest)
    char *argv[] = {(char *)"doom", (char *)"-iwad", (char *)WAD_NAME, (char *)"-mb", (char *)ZONE_MIB, NULL};
#if DOOM_SPIKE
    spike_calibrate();
#endif
    loading = true;
    doomgeneric_Create(5, argv);
    loading = false;
#if DOOM_SPIKE
    spike_t_begin = lv_tick_get();
#endif
    while (!stop_req) {
        if (doom_ui_engine_held()) {
            vTaskDelay(pdMS_TO_TICKS(20)); // the OS has the screen (#564): the game waits, the tick is not run
            continue;
        }
#if DOOM_SPIKE
        bool     was_level = gamestate == SPIKE_GS_LEVEL;
        doom_spike_frame_begin(&spike);
        doomgeneric_Tick();
        doom_spike_frame_end(&spike, was_level && gamestate == SPIKE_GS_LEVEL);
        spike_t_end = lv_tick_get(); // the wall clock stops at the last frame, not at the stop's wait
#else
        doomgeneric_Tick();
#endif
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
    uint32_t begin = lv_tick_get(); // not xTaskGetTickCount: it is not in the export table (#550)
    bool started = start_state == START_OK;
    for (int waited = 0; started && !engine_gone() && waited < STOP_WAIT; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (started && !engine_gone()) {
        return false; // the engine task is stuck: the loader reports it, nothing is freed under a running task
    }
    // task_done is the engine's last word, not its end: the task is still returning. Wait until the OS's book has no task of this
    // app left, so the unload never frees the image under it (bench #564: a paused engine stopped at once and was still booked).
    for (int waited = 0; started && lichen_task_leftovers(lichen_ctx_self(), 0) != 0 && waited < STOP_END_WAIT; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    uint32_t spent = lv_tick_elaps(begin);
    if (!doom_ui_detach(spent < STOP_TOTAL ? STOP_TOTAL - spent : 200)) {
        return false; // the LVGL task still has the picture's memory: nothing is freed under it
    }
    char summary[128];
#if DOOM_SPIKE
    spike_summary(summary, sizeof(summary));
#else
    doom_ui_summary(summary, sizeof(summary));
#endif
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
    doom_ui_init(size);
    if (size) {
        start_state = lichen_task_start(lichen_ctx_self(), "doom", engine_task, NULL, STACK_BYTES, ENGINE_PRIO, NULL)
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
