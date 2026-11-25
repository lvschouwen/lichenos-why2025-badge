// The DOOM app's screen (#526, #564), see doom_ui.h. Two paths, chosen when the first frame has come (doom_path.h): the OS
// video surface (lichen_video.h: the engine task expands the 8-bit frame through a 256-entry RGB565 LUT into the OS's source
// buffer and presents it, no LVGL involved) or, when the OS refuses it, the #526 canvas below. Video, canvas path: the engine's 8-bit 320x200 frame is doubled to 640x400 on the engine
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
#include "doom_path.h"

#include "hold_exit.h"
#include "lichen_app.h"
#include "lichen_keys.h"
#include "key_matrix.h"
#include "lichen_mem.h"
#include "lichen_ui.h"
#include "lichen_video.h"
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
    lv_obj_t   *pause_label;
    lv_obj_t   *pause_box;   // the focusable card of the pause: Enter on it resumes
    lv_timer_t *timer;
    bool        failed;      // the picture has no memory: the screen says so, nothing is retried
    bool        pause_shown;
    bool        resume_req;  // the person chose Resume
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

static doom_path_t         path;        // which way the picture goes (the LVGL task writes, the engine reads)
static atomic_bool         first_frame; // the engine finished a frame while the path was still undecided
static atomic_bool         parked;      // the engine saw the pause and draws nothing: the video buffers are free to release
static atomic_bool         opened;      // doom_ui_open built the screen: detach has the timer, keys and surface to give back
static atomic_bool         video_held;  // lichen_video_acquire succeeded and release has not (detach gives it back)
static void               *video_mem;   // PSRAM block of both canvas pictures (and their palettes); canvas path only
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

static void on_key(lichen_raw_key_t const *key, void *arg);
static bool build_picture(lv_obj_t *screen);

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

_Static_assert(DOOM_RECLAIM_ESCAPE == (int)LICHEN_VIDEO_RECLAIM_ESCAPE, "doom_path.h follows lichen_video.h");

// LVGL task, under the UI lock: the OS took the screen. Flags only (the contract): the tick shows the pause and releases.
static void on_reclaim(void *arg, lichen_video_reclaim_t why) {
    (void)arg;
    if (doom_path_reclaimed(&path, (int)why)) {
        ui.hold        = (hold_exit_t){0}; // the OS takes the raw keys back: an Esc-down never gets its release
        ui.pause_shown = false;
        ui.resume_req  = false;
    }
}

// The picture's way is chosen once, when the first frame has come: the OS surface, else the #526 canvas
static void show_pause_text(char const *text);

// The picture could not be shown: the person is told where they are looking (the loading text, or the pause card on a resume)
static void fail_picture(char const *text) {
    ui.failed = true;
    if (doom_path_get(&path) == DOOM_PATH_PAUSED) {
        show_pause_text(text);
        return;
    }
    lv_obj_set_hidden(ui.load_label, false);
    lichen_label_set(ui.load_label, text);
}

static void begin_picture(void) {
    switch_to_picture();
    doom_fps_reset(&ui.fps); // a resume starts a new segment: the pause is not a stalled second, the notice covers the last one
    lichen_video_cfg_t cfg = {.src_w = DOOM_SRC_W, .src_h = DOOM_SRC_H, .on_reclaim = on_reclaim, .arg = NULL};
    if (lichen_video_acquire(&cfg)) {
        atomic_store(&video_held, true);
        atomic_store(&parked, false);
        doom_path_chosen(&path, true);
        return;
    }
    if (!build_picture(ui.screen)) {
        fail_picture("DOOM has no memory for its picture. Close it and open it again.");
        return;
    }
    lv_obj_set_hidden(ui.canvas, false);
    doom_path_chosen(&path, false);
}

static void show_pause_text(char const *text) {
    ui.pause_shown = true;
    lichen_label_set(ui.pause_label, text);
    lv_obj_set_hidden(ui.pause_label, false);
    lv_obj_set_hidden(ui.pause_box, false);
}

static void show_pause(bool on) {
    if (on) {
        lichen_label_set(ui.pause_label, "Paused. Press Enter to go back to the game.");
    }
    ui.pause_shown = on;
    lv_obj_set_hidden(ui.pause_label, !on);
    lv_obj_set_hidden(ui.pause_box, !on);
    if (on && lv_obj_get_group(ui.pause_box)) {
        lv_group_focus_obj(ui.pause_box);
    }
}

// LVGL task: the OS had the screen (a dialog, the thermal warning). The engine waits; Enter takes the screen again.
static void tick_paused(void) {
    if (doom_path_why(&path) == DOOM_RECLAIM_ESCAPE) {
        // The OS pops the app's screen itself (only while it is still on top, #563); if it does not, the app's own hold-Esc is
        // not offered here either: the pause card stays and Esc held 2 s is the OS's again once the screen is closed
        return;
    }
    if (ui.failed) {
        return; // the card says why the game cannot come back
    }
    if (!ui.pause_shown) {
        show_pause(true);
    }
    if (!ui.resume_req || !atomic_load(&parked)) {
        return; // the engine may still be inside a frame: the buffers are released only once it stopped
    }
    if (!lichen_video_release()) {
        return; // the UI lock was busy: the request stays
    }
    atomic_store(&video_held, false);
    ui.resume_req = false;
    show_pause(false);
    if (!lichen_keys_raw_take(on_key, NULL)) { // the OS took the keys back with the screen
        fail_picture("DOOM could not take the keyboard (another app may hold it). Close it and open DOOM again.");
        return;
    }
    begin_picture();
}

static void tick_canvas(uint32_t now) {
    if (atomic_load_explicit(&ready, memory_order_acquire)) {
        take_frame();
    } else if (ui.fps.started) {
        doom_fps_advance(&ui.fps, now); // a stalled engine or display shows as seconds of 0 fps
    }
    if (ui.fps_shown && ui.fps.windows != ui.label_windows) {
        ui.label_windows = ui.fps.windows;
        show_fps_label();
    }
}

static void screen_tick(lv_timer_t *t) {
    (void)t;
    if (!ui.screen) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (hold_exit_tick(&ui.hold, now)) {
        // Esc held leaves (a short Esc is the game's menu), but while the OS surface is held, or was reclaimed for the OS's
        // own hold-Esc, the OS pops this screen itself: a second shell_back would pop the screen below it (#563 review)
        if (doom_path_own_back_ok(&path)) {
            shell_back();
        }
        return;
    }
    switch (doom_path_get(&path)) {
    case DOOM_PATH_PENDING:
        if (ui.failed) {
            break;
        }
        if (atomic_load(&first_frame)) {
            begin_picture();
        } else {
            show_load_progress();
        }
        break;
    case DOOM_PATH_VIDEO:
        break; // LVGL has nothing to do: its strips under the picture are dropped by the OS, so the label and widgets stay off
    case DOOM_PATH_PAUSED:
        tick_paused();
        break;
    case DOOM_PATH_CANVAS:
        tick_canvas(now);
        break;
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
    doom_path_state_t st = doom_path_get(&path);
    if (st != DOOM_PATH_VIDEO && st != DOOM_PATH_CANVAS) {
        return; // nothing to play yet (keys typed during the WAD load must not replay at the first frame) or paused
    }
    if (key->code == DOOM_CODE_FPS_TOGGLE) {
        if (key->pressed && st == DOOM_PATH_CANVAS) { // the label is LVGL's: it has no place under the video
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
    ui.pause_label = NULL;
    ui.pause_box  = NULL;
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

static void on_pause_key(lv_event_t *e) {
    if (lv_event_get_key(e) == LV_KEY_ENTER) {
        ui.resume_req = true; // the tick resumes once the engine has stopped drawing
    }
}

// The pause card (hidden until the OS takes the screen): text, and a focusable box that takes Enter. Not a button: apps have none
static void build_pause(lv_obj_t *screen) {
    ui.pause_box = lv_obj_create(screen);
    lv_obj_set_size(ui.pause_box, 560, 160);
    lv_obj_set_style_bg_opa(ui.pause_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.pause_box, 2, 0);
    lv_obj_set_style_border_color(ui.pause_box, lv_color_hex(THEME_AMBER), 0);
    lv_obj_align(ui.pause_box, LV_ALIGN_CENTER, 0, 0);
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, ui.pause_box);
    }
    lichen_ui_event_add(ui.pause_box, on_pause_key, LV_EVENT_KEY, NULL);
    ui.pause_label = lv_label_create(ui.pause_box);
    lv_label_set_text(ui.pause_label, "Paused. Press Enter to go back to the game.");
    lv_label_set_long_mode(ui.pause_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui.pause_label, 500);
    lv_obj_set_style_text_color(ui.pause_label, lv_color_hex(THEME_TEXT), 0);
    lv_obj_center(ui.pause_label);
    lv_obj_set_hidden(ui.pause_box, true);
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
    build_pause(screen);
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
    // the screen has no padding (the picture fills it), so the hint carries the THEME_PAD edges itself (UI rule 7, #554)
    lv_obj_t *hint = shell_page_hint(screen, "Esc", "menu", "hold Esc", "leave", NULL);
    lv_obj_set_style_pad_hor(hint, THEME_PAD, 0);
    atomic_store(&present_on, true);
    atomic_store(&opened, true);
    return true;
}

// ---- the engine's side ---------------------------------------------------------------------------------------------------

void doom_ui_init(uint32_t bytes) {
    doom_keyq_init(&keyq); // before the engine task starts: it reads the ring from its first tick
    atomic_store(&wad_size, bytes);
    doom_path_init(&path);
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

// The video path (#564): the frame through the palette's LUT into the OS's source buffer, 320x200 (the OS scales it). A dropped
// present (the OS still shows the last one) leaves the buffer as it is: the next frame renders into it again.
static void present_video(void) {
    uint16_t *dst = lichen_video_back();
    if (!dst) {
        return; // reclaimed or released under this frame
    }
    if (palette_changed || !pal_ready) {
        refresh_palette(); // the LUT is rebuilt only when the engine's palette changed
    }
    doom_expand_1x(dst, DG_ScreenBuffer, pal565);
    if (lichen_video_present()) {
        doom_fps_frame(&ui.fps, lv_tick_get()); // the engine task is the only writer while the surface is held
    }
}

static void present_canvas(void) {
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

void doom_ui_present(void) {
    if (!atomic_load(&present_on) || !DG_ScreenBuffer) {
        return;
    }
    switch (doom_path_get(&path)) {
    case DOOM_PATH_PENDING:
        atomic_store(&first_frame, true); // the screen chooses the path; this frame is dropped
        break;
    case DOOM_PATH_VIDEO:
        present_video();
        break;
    case DOOM_PATH_CANVAS:
        present_canvas();
        break;
    case DOOM_PATH_PAUSED:
        break;
    }
}

// Engine task only: the pause is taken out of the game's clock, or its tic loop would run the missed time in one burst
static uint32_t pause_begin, paused_total;
static bool     in_pause;

bool doom_ui_engine_held(void) {
    bool held = doom_path_get(&path) == DOOM_PATH_PAUSED;
    if (held && !in_pause) {
        pause_begin = lv_tick_get();
    } else if (!held && in_pause) {
        paused_total += lv_tick_get() - pause_begin;
    }
    in_pause = held;
    atomic_store(&parked, held); // seen between two frames: the video buffers are not being written
    return held;
}

uint32_t doom_ui_game_ms(void) {
    return lv_tick_get() - paused_total;
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
    if (atomic_load(&video_held)) {
        if (!lichen_video_release()) {
            return; // the UI lock was busy: detached stays false, the loader's wait runs out and the stop is reported
        }
        atomic_store(&video_held, false);
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
    if (!atomic_load(&opened)) {
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
    if (video_mem) {
        lichen_free(lichen_ctx_self(), video_mem);
        video_mem = NULL;
    }
    atomic_store(&opened, false);
    return true;
}

void doom_ui_summary(char *out, size_t cap) {
    snprintf(out, cap, "%s", ui.summary);
}
