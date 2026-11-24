// The DOOM app's screen (#526), see doom_ui.h. Video: the engine's 8-bit 320x200 frame is expanded to 640x400 RGB565 into the
// canvas's own buffer on the engine task; LVGL then copies it straight into its render buffer. An I8 canvas with the palette
// (lv_canvas_set_palette) would halve the buffer, but LVGL's software renderer cannot draw an indexed image directly:
// lv_bin_decoder.c decodes it line by line to ARGB8888 on every refresh (LV_BIN_DECODER_RAM_LOAD is off, a whole-image
// decode would be 1 MB), so the conversion this file does once per frame would be done by LVGL as well, once per strip.
// All of it sits behind doom_ui_present, so the other format is a change in two functions (#526).
// GPL-3.0-or-later, like the app as a whole.
#include "doom_ui.h"

#include "doom_engine.h"
#include "doom_logic.h"

#include "lichen_app.h"
#include "lichen_keys.h"
#include "lichen_mem.h"
#include "lichen_ui.h"
#include "theme.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define PIC_W      (DOOM_SRC_W * 2)
#define PIC_H      (DOOM_SRC_H * 2)
#define PIC_BYTES  ((size_t)PIC_W * PIC_H * 2)
#define TIMER_MS   16   // the screen's poll: a frame is on the glass within a refresh of the engine finishing it
#define DETACH_WAIT 1500 // ms the LVGL task gets to take the canvas and keys off before stop gives up

static struct {
    lv_obj_t   *screen;
    lv_obj_t   *canvas;
    lv_obj_t   *load_label;
    lv_obj_t   *bar;
    lv_obj_t   *pct;
    lv_obj_t   *fps_label;
    lv_timer_t *timer;
    bool        picture;     // the first frame has come: the canvas shows, the loading widgets are gone
    bool        fps_shown;
    uint32_t    shown_seq;
    uint32_t    shown_pct;
    uint32_t    label_windows;
    doom_fps_t  fps;
    uint32_t    load_kib;    // KiB of the WAD read when the first frame came
    char        summary[112]; // the exit notice's text (a notice body holds 120), written by detach_cb
} ui; // the LVGL task's, and doom_ui_summary after the detach

static void                  *video_mem;       // PSRAM block: LVGL's own pointer is the aligned start
static _Atomic(uint16_t *)    video_pix;       // the canvas's pixels, NULL until open and again after detach
static _Atomic uint32_t       frame_seq;       // frames the engine finished
static _Atomic uint32_t       wad_size;
static _Atomic uint32_t       bytes_read;
static atomic_bool            detached;
static doom_keyq_t            keyq;
static uint16_t               pal565[256];     // engine task only
static bool                   pal_ready;

// ---- the screen (LVGL task) ----------------------------------------------------------------------------------------------

static void show_fps_label(void) {
    char text[48];
    doom_fps_t const *f = &ui.fps;
    uint32_t avg = doom_fps_avg_x10(f);
    snprintf(text, sizeof(text), "%u fps   avg %u.%u   low %u   %us", (unsigned)f->last_win, (unsigned)(avg / 10),
             (unsigned)(avg % 10), (unsigned)doom_fps_min(f), (unsigned)f->windows);
    lichen_label_set(ui.fps_label, f->windows ? text : "fps: counting...");
}

static void switch_to_picture(void) {
    ui.picture  = true;
    ui.load_kib = atomic_load(&bytes_read) >> 10;
    lv_obj_set_hidden(ui.load_label, true);
    lv_obj_set_hidden(ui.bar, true);
    lv_obj_set_hidden(ui.pct, true);
    lv_obj_set_hidden(ui.canvas, false);
}

static void show_load_progress(void) {
    uint32_t pct = doom_load_percent(atomic_load(&bytes_read), atomic_load(&wad_size));
    if (pct == ui.shown_pct) {
        return;
    }
    ui.shown_pct = pct;
    char text[8];
    snprintf(text, sizeof(text), "%u%%", (unsigned)pct);
    lv_bar_set_value(ui.bar, (int32_t)pct, LV_ANIM_OFF);
    lichen_label_set(ui.pct, text);
}

static void screen_tick(lv_timer_t *t) {
    (void)t;
    if (!ui.screen) {
        return;
    }
    uint32_t seq = atomic_load_explicit(&frame_seq, memory_order_acquire);
    if (!ui.picture) {
        if (seq == 0) {
            show_load_progress();
            return;
        }
        switch_to_picture();
    }
    uint32_t now = lv_tick_get();
    if (seq != ui.shown_seq) {
        ui.shown_seq = seq;
        lv_obj_invalidate(ui.canvas);
        doom_fps_frame(&ui.fps, now);
    } else {
        doom_fps_advance(&ui.fps, now); // a stalled engine shows as seconds of 0 fps
    }
    if (ui.fps_shown && ui.fps.windows != ui.label_windows) {
        ui.label_windows = ui.fps.windows;
        show_fps_label();
    }
}

static void toggle_fps(void) {
    ui.fps_shown = !ui.fps_shown;
    doom_fps_reset(&ui.fps); // each press starts a measurement: "60 s of E1M1" begins where the label comes on
    ui.label_windows = 0;
    lv_obj_set_hidden(ui.fps_label, !ui.fps_shown);
    if (ui.fps_shown) {
        show_fps_label();
    }
}

// LVGL task: the key callback only maps and queues, the engine reads the queue on its own task
static void on_key(lichen_raw_key_t const *key, void *arg) {
    (void)arg;
    if (key->code == DOOM_CODE_FPS_TOGGLE) {
        if (key->pressed && ui.screen) {
            toggle_fps();
        }
        return;
    }
    uint8_t k = doom_key_for_code(key->code);
    if (k != DOOM_KEY_NONE) {
        doom_keyq_push(&keyq, key->pressed, k);
    }
}

static void on_screen_delete(lv_event_t *e) {
    (void)e;
    lichen_keys_raw_release(on_key, NULL);
    if (ui.timer) {
        lv_timer_delete(ui.timer);
        ui.timer = NULL;
    }
    // the children go with the screen; the picture's memory stays until stop, the engine may still write into it
    ui.screen     = NULL;
    ui.canvas     = NULL;
    ui.load_label = NULL;
    ui.bar        = NULL;
    ui.pct        = NULL;
    ui.fps_label  = NULL;
}

static bool build_picture(lv_obj_t *screen) {
    // 64: a cache line of the P4 apart from the neighbours; PSRAM, the engine writes it and LVGL's render reads it
    video_mem = lichen_alloc_aligned(lichen_ctx_self(), 64, PIC_BYTES, LICHEN_MEM_BULK);
    if (!video_mem) {
        return false;
    }
    memset(video_mem, 0, PIC_BYTES);
    ui.canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(ui.canvas, video_mem, PIC_W, PIC_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(ui.canvas, LV_ALIGN_CENTER, 0, THEME_STATUS_BAR_H / 2); // centred in what is left under the status bar
    lv_obj_set_hidden(ui.canvas, true);
    atomic_store(&video_pix, (uint16_t *)video_mem);
    return true;
}

static void build_loading(lv_obj_t *screen) {
    ui.load_label = lv_label_create(screen);
    lv_label_set_text(ui.load_label, "Loading the WAD...");
    lv_label_set_long_mode(ui.load_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui.load_label, 600);
    lv_obj_set_style_text_align(ui.load_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ui.load_label, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(ui.load_label, LV_ALIGN_CENTER, 0, -40);
    ui.bar = lv_bar_create(screen);
    lv_bar_set_range(ui.bar, 0, 100);
    lv_obj_set_size(ui.bar, 480, 24);
    lv_obj_set_style_bg_color(ui.bar, lv_color_hex(THEME_TRACK), 0);
    lv_obj_align(ui.bar, LV_ALIGN_CENTER, 0, 10);
    ui.pct = lv_label_create(screen);
    lv_label_set_text(ui.pct, "0%");
    lv_obj_set_style_text_color(ui.pct, lv_color_hex(THEME_DIM), 0);
    lv_obj_align(ui.pct, LV_ALIGN_CENTER, 0, 50);
}

bool doom_ui_open(lv_obj_t *screen) {
    doom_keyq_init(&keyq);
    doom_fps_reset(&ui.fps);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui.screen = screen;
    build_loading(screen);
    ui.fps_label = lv_label_create(screen);
    lv_obj_set_style_text_color(ui.fps_label, lv_color_hex(THEME_AMBER), 0);
    lv_obj_align(ui.fps_label, LV_ALIGN_TOP_LEFT, THEME_PAD, THEME_STATUS_BAR_H + 8); // below the status bar
    lv_obj_set_hidden(ui.fps_label, true);
    shell_page_hint(screen, "Esc", "menu", "Ctrl", "fire", "Space", "use", NULL);
    if (!build_picture(screen)) {
        lv_label_set_text(ui.load_label, "DOOM has no memory for its picture. Close it and open it again.");
        lv_obj_set_hidden(ui.bar, true);
        lv_obj_set_hidden(ui.pct, true);
        return false;
    }
    lichen_ui_event_add(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
    ui.timer = lichen_ui_timer_create(screen_tick, TIMER_MS, NULL);
    if (!lichen_keys_raw_take(on_key, NULL)) {
        lv_label_set_text(ui.load_label, "The keys are in use by another app. Close it and open DOOM again.");
    }
    return true;
}

// ---- the engine's side ---------------------------------------------------------------------------------------------------

void doom_ui_set_wad_size(uint32_t bytes) {
    atomic_store(&wad_size, bytes);
}

void doom_ui_note_read(size_t n) {
    uint32_t cur = atomic_load(&bytes_read);
    atomic_store(&bytes_read, n > UINT32_MAX - cur ? UINT32_MAX : cur + (uint32_t)n); // one writer: the engine task
}

static void refresh_palette(void) {
    for (int i = 0; i < 256; i++) {
        pal565[i] = doom_rgb565((uint8_t)colors[i].r, (uint8_t)colors[i].g, (uint8_t)colors[i].b);
    }
    palette_changed = 0;
    pal_ready       = true;
}

void doom_ui_present(void) {
    uint16_t *pix = atomic_load(&video_pix);
    if (!pix || !DG_ScreenBuffer) {
        return;
    }
    if (palette_changed || !pal_ready) {
        refresh_palette();
    }
    doom_expand_2x(pix, DG_ScreenBuffer, pal565);
    atomic_fetch_add_explicit(&frame_seq, 1, memory_order_release);
}

bool doom_ui_next_key(int *pressed, unsigned char *key) {
    bool    p;
    uint8_t k;
    if (!doom_keyq_pop(&keyq, &p, &k)) {
        return false;
    }
    *pressed = p;
    *key     = k;
    return true;
}

// ---- stop ----------------------------------------------------------------------------------------------------------------

// LVGL task: nothing reads the picture's memory after this ran (the render is on this task)
static void detach_cb(void *arg) {
    (void)arg;
    lichen_keys_raw_release(on_key, NULL);
    if (ui.timer) {
        lv_timer_delete(ui.timer);
        ui.timer = NULL;
    }
    if (ui.screen) {
        doom_fps_advance(&ui.fps, lv_tick_get());
    }
    uint32_t avg = doom_fps_avg_x10(&ui.fps);
    // the WAD figure is for calibrating the loading bar: how much of the file the engine reads before its first frame
    snprintf(ui.summary, sizeof(ui.summary), "%u.%u fps on average, lowest second %u, over %u s. Read %u of %u KiB to start.",
             (unsigned)(avg / 10), (unsigned)(avg % 10), (unsigned)doom_fps_min(&ui.fps), (unsigned)ui.fps.windows,
             (unsigned)ui.load_kib, (unsigned)(atomic_load(&wad_size) >> 10));
    if (!ui.fps.windows) {
        ui.summary[0] = '\0'; // nothing was played: no notice
    }
    if (ui.canvas) {
        lv_obj_delete(ui.canvas);
        ui.canvas = NULL;
    }
    atomic_store(&detached, true);
}

bool doom_ui_detach(void) {
    if (!video_mem) {
        return true;
    }
    atomic_store(&video_pix, NULL);
    atomic_store(&detached, false);
    if (!lichen_ui_call(detach_cb, NULL)) {
        return false;
    }
    for (int waited = 0; !atomic_load(&detached) && waited < DETACH_WAIT; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!atomic_load(&detached)) {
        return false;
    }
    lichen_free(lichen_ctx_self(), video_mem);
    video_mem = NULL;
    return true;
}

void doom_ui_summary(char *out, size_t cap) {
    snprintf(out, cap, "%s", ui.summary);
}
