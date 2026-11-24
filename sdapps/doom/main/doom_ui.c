// The DOOM app's screen (#526), see doom_ui.h. Video: the engine's 8-bit 320x200 frame is doubled to 640x400 on the engine
// task into the canvas's own buffer, and LVGL draws that canvas. Two formats, one switch (-DDOOM_VIDEO_RGB565):
//   I8 (default, 2 x 256 KB): the indexes stay indexes and the palette sits in front of them in the buffer (LVGL's layout of
//     an indexed buffer: 256 x BGRA, the engine task copies it in with every frame). LVGL's software renderer cannot blend an indexed image directly: with LV_BIN_DECODER_RAM_LOAD off
//     (the firmware's config) lv_bin_decoder.c decodes it line by line to ARGB8888 while drawing, so it is the slower of the
//     two per refresh. It was not measured (no bench in a build-only run): the first bench measures it.
//   RGB565 (2 x 512 KB): the engine task looks the palette up while doubling, LVGL copies the canvas straight into its render
//     buffer. The fallback when I8 does not reach 30 fps.
// Two buffers: the engine only writes the one LVGL is not showing, and LVGL takes a finished one on its own task between
// renders (lv_canvas_set_buffer), so a frame never tears. When LVGL has not taken the last frame yet the engine drops its
// next one rather than touch a buffer that may be drawn.
// GPL-3.0-or-later, like the app as a whole.
#include "doom_ui.h"

#include "doom_engine.h"
#include "doom_logic.h"

#include "hold_exit.h"
#include "lichen_app.h"
#include "lichen_keys.h"
#include "key_matrix.h"
#include "lichen_mem.h"
#include "lichen_ui.h"
#include "theme.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define PIC_W     (DOOM_SRC_W * 2)
#define PIC_H     (DOOM_SRC_H * 2)
#define TIMER_MS  16 // the screen's poll: a frame is handed to LVGL within a tick of the engine finishing it

#ifdef DOOM_VIDEO_RGB565
#define PIC_CF       LV_COLOR_FORMAT_RGB565
#define PIC_PREFIX   0                    // bytes before the pixels
#define PIC_BYTES    ((size_t)PIC_W * PIC_H * 2)
#else
#define PIC_CF       LV_COLOR_FORMAT_I8
#define PIC_PREFIX   1024                 // LVGL keeps the 256-entry palette in front of the pixels of an indexed buffer
#define PIC_BYTES    ((size_t)PIC_W * PIC_H)
#endif

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
    bool        draw_due;    // a new frame was invalidated and no draw of it has been counted yet
    uint32_t    shown_pct;
    uint32_t    label_windows;
    uint32_t    load_kib;    // KiB of the WAD read when the first frame came
    doom_fps_t  fps;
    hold_exit_t hold;        // Esc held leaves: raw keys take the OS's own Esc away
    char        summary[112]; // the exit notice's text (a notice body holds 120), written by detach_cb
} ui; // the LVGL task's, and doom_ui_summary after the detach

static void               *video_mem;   // PSRAM block of both pictures (and their palettes)
static uint8_t            *bufs[2];     // each: PIC_PREFIX bytes of palette, then the pixels; set in open, before the engine draws
static atomic_bool         present_on;  // the engine may draw: open built the buffers, detach has not started
static atomic_bool         ready;       // the engine published bufs[last] and LVGL has not taken it yet
static int                 last = 1;    // the buffer published last; the canvas shows it once ready is false
static _Atomic uint32_t    wad_size;
static _Atomic uint32_t    bytes_read;
static atomic_bool         detached;
static doom_keyq_t         keyq;
static uint32_t            pal32[256];  // engine task only: 0xAARRGGBB, which is LVGL's BGRA bytes on this little-endian core (I8)
static uint16_t            pal565[256]; // engine task only (RGB565)
static bool                pal_ready;

// ---- the screen (LVGL task) ----------------------------------------------------------------------------------------------

static void show_fps_label(void) {
    char              text[48];
    doom_fps_t const *f   = &ui.fps;
    uint32_t          avg = doom_fps_avg_x10(f);
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

// What the glass shows: counted when LVGL draws the canvas for a frame this screen invalidated, not when the engine made
// it (the engine runs at 35 Hz, LVGL refreshes at its own period; a strip-wise render calls this per strip, once counts).
// Two frames taken between two draws count once: the count is what the glass shows, so it reads low rather than high.
static void on_canvas_draw(lv_event_t *e) {
    (void)e;
    if (ui.draw_due) {
        ui.draw_due = false;
        doom_fps_frame(&ui.fps, lv_tick_get());
    }
}

// LVGL task: shows the buffer the engine finished
static void take_frame(void) {
    // Alternating the canvas's buffer is safe only because LVGL's image cache is off (CONFIG_LV_CACHE_DEF_SIZE=0, a cached
    // decode of the old pointer would show stale) and no async draw unit (PPA, a draw thread) reads the canvas after the
    // refresh returns; this task is the renderer, so a swap here is between renders. Revisit if either changes.
    lv_canvas_set_buffer(ui.canvas, bufs[last], PIC_W, PIC_H, PIC_CF);
    ui.draw_due = true;
    lv_obj_invalidate(ui.canvas);
    atomic_store_explicit(&ready, false, memory_order_release); // the other buffer is the engine's again
}

static void screen_tick(lv_timer_t *t) {
    (void)t;
    if (!ui.screen) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (hold_exit_tick(&ui.hold, now)) {
        shell_back(); // Esc held: out of DOOM (a short Esc is the game's menu)
        return;
    }
    bool frame = atomic_load_explicit(&ready, memory_order_acquire);
    if (!ui.picture) {
        if (!frame) {
            show_load_progress();
            return;
        }
        switch_to_picture();
    }
    if (frame) {
        take_frame();
    } else if (ui.fps.started) {
        doom_fps_advance(&ui.fps, now); // a stalled engine or display shows as seconds of 0 fps
    }
    if (ui.fps_shown && ui.fps.windows != ui.label_windows) {
        ui.label_windows = ui.fps.windows;
        show_fps_label();
    }
}

static void toggle_fps(void) {
    ui.fps_shown = !ui.fps_shown;
    lv_obj_set_hidden(ui.fps_label, !ui.fps_shown);
    if (ui.fps_shown) {
        doom_fps_reset(&ui.fps); // each time the label comes on starts a measurement: "60 s of E1M1" begins there
        ui.label_windows = 0;
        show_fps_label();
    }
}

// LVGL task: the key callback only maps and queues, the engine reads the queue on its own task
static void on_key(lichen_raw_key_t const *key, void *arg) {
    (void)arg;
    hold_exit_event(&ui.hold, key->code == KEY_MATRIX_ESC_CODE, key->pressed, lv_tick_get());
    if (!ui.picture) {
        return; // nothing to play yet: keys typed during the WAD load must not replay at the first frame
    }
    if (key->code == DOOM_CODE_FPS_TOGGLE) {
        if (key->pressed) {
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
    size_t one = PIC_PREFIX + PIC_BYTES; // a multiple of 64 in both formats: the second buffer stays aligned
    // 64: a cache line of the P4 apart from the neighbours; PSRAM, the engine writes it and LVGL's render reads it
    video_mem = lichen_alloc_aligned(lichen_ctx_self(), 64, 2 * one, LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!video_mem) {
        return false;
    }
    bufs[0]   = video_mem;
    bufs[1]   = (uint8_t *)video_mem + one;
    ui.canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(ui.canvas, bufs[last], PIC_W, PIC_H, PIC_CF); // the engine starts on the other one
    lv_obj_align(ui.canvas, LV_ALIGN_CENTER, 0, THEME_STATUS_BAR_H / 2); // centred in what is left under the status bar
    lv_obj_set_hidden(ui.canvas, true);
    lichen_ui_event_add(ui.canvas, on_canvas_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
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

// The screen's way out is its own (raw keys take the OS's Esc away): the timer runs hold-Esc, so the keys are taken only
// once it exists, and a screen that cannot have both leaves the keys alone
static bool fail_open(char const *text) {
    atomic_store(&present_on, false);
    lv_label_set_text(ui.load_label, text);
    lv_obj_set_hidden(ui.bar, true);
    lv_obj_set_hidden(ui.pct, true);
    return false;
}

bool doom_ui_open(lv_obj_t *screen) {
    doom_fps_reset(&ui.fps);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui.screen = screen;
    build_loading(screen);
    ui.fps_label = lv_label_create(screen);
    lv_obj_set_style_text_color(ui.fps_label, lv_color_hex(THEME_AMBER), 0);
    lv_obj_align(ui.fps_label, LV_ALIGN_TOP_LEFT, THEME_PAD, THEME_STATUS_BAR_H + 8); // below the status bar
    lv_obj_set_hidden(ui.fps_label, true);
    lichen_ui_event_add(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
    if (!build_picture(screen)) {
        return fail_open("DOOM has no memory for its picture. Close it and open it again.");
    }
    ui.timer = lichen_ui_timer_create(screen_tick, TIMER_MS, NULL);
    if (!ui.timer) {
        return fail_open("DOOM has no memory for its screen. Close it and open it again.");
    }
    if (!lichen_keys_raw_take(on_key, NULL)) {
        lv_timer_delete(ui.timer);
        ui.timer = NULL;
        return fail_open("DOOM could not take the keyboard (another app may hold it). Close it and open DOOM again.");
    }
    // only with the keys held does Esc mean the game's menu; the failed screens above leave Esc as the OS's own
    shell_page_hint(screen, "Esc", "menu", "hold Esc", "leave", NULL);
    atomic_store(&present_on, true);
    return true;
}

// ---- the engine's side ---------------------------------------------------------------------------------------------------

void doom_ui_init(uint32_t bytes) {
    doom_keyq_init(&keyq); // before the engine task starts: it reads the ring from its first tick
    atomic_store(&wad_size, bytes);
}

void doom_ui_note_read(size_t n) {
    uint32_t cur = atomic_load(&bytes_read);
    atomic_store(&bytes_read, n > UINT32_MAX - cur ? UINT32_MAX : cur + (uint32_t)n); // one writer: the engine task
}

// the engine's palette (i_video.c's colors[]) into the form each format needs
static void refresh_palette(void) {
    for (int i = 0; i < 256; i++) {
        pal565[i] = doom_rgb565((uint8_t)colors[i].r, (uint8_t)colors[i].g, (uint8_t)colors[i].b);
        pal32[i]  = 0xff000000u | (uint32_t)colors[i].r << 16 | (uint32_t)colors[i].g << 8 | colors[i].b;
    }
    palette_changed = 0;
    pal_ready       = true;
}

void doom_ui_present(void) {
    if (!atomic_load(&present_on) || !DG_ScreenBuffer) {
        return;
    }
    if (atomic_load_explicit(&ready, memory_order_acquire)) {
        return; // LVGL has not taken the last frame: it may be about to draw the buffer this one would go into
    }
    if (palette_changed || !pal_ready) {
        refresh_palette();
    }
    int      b   = 1 - last;
    uint8_t *pix = bufs[b] + PIC_PREFIX;
#ifdef DOOM_VIDEO_RGB565
    doom_expand_2x(pix, DG_ScreenBuffer, pal565);
#else
    memcpy(bufs[b], pal32, sizeof(pal32)); // the palette in front of the pixels, with the frame it belongs to
    doom_expand_2x_i8(pix, DG_ScreenBuffer);
#endif
    last = b;
    atomic_store_explicit(&ready, true, memory_order_release);
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
    if (ui.canvas) {
        lv_obj_delete(ui.canvas);
        ui.canvas = NULL;
    }
    uint32_t avg = doom_fps_avg_x10(&ui.fps);
    // the WAD figure is for calibrating the loading bar: how much of the file the engine reads before its first frame
    snprintf(ui.summary, sizeof(ui.summary), "%u.%u fps on average, lowest second %u, over %u s. Read %u of %u KiB to start.",
             (unsigned)(avg / 10), (unsigned)(avg % 10), (unsigned)doom_fps_min(&ui.fps), (unsigned)ui.fps.windows,
             (unsigned)ui.load_kib, (unsigned)(atomic_load(&wad_size) >> 10));
    if (!ui.fps.windows) {
        ui.summary[0] = '\0'; // nothing was measured: no notice
    }
    atomic_store(&detached, true);
}

// When the wait runs out, detach_cb may still be queued: the OS must cancel or drain the app's queued UI calls before it
// unloads the image (#541). Nothing is freed here in that case, so a late run only repeats what it did.
bool doom_ui_detach(uint32_t wait_ms) {
    if (!video_mem) {
        return true;
    }
    atomic_store(&present_on, false);
    atomic_store(&detached, false);
    if (!lichen_ui_call(detach_cb, NULL)) {
        return false;
    }
    for (uint32_t waited = 0; !atomic_load(&detached) && waited < wait_ms; waited += 10) {
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
