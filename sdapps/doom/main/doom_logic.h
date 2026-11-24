#pragma once
// The pure half of the DOOM app's video and keys (#526): no ESP or LVGL, so host_tests/doom_logic_test.c runs it. Key codes
// are the keyboard's (TCA8418, lichen_api key_matrix.h); the DOOM key values are doomkeys.h's, repeated here because an
// app's own sources may include only its own headers (tools/check_sdapp_boundary.py).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- keys ----------------------------------------------------------------------------------------------------------------

#define DOOM_KEY_NONE       0
#define DOOM_KEY_RIGHTARROW 0xae
#define DOOM_KEY_LEFTARROW  0xac
#define DOOM_KEY_UPARROW    0xad
#define DOOM_KEY_DOWNARROW  0xaf
#define DOOM_KEY_STRAFE_L   0xa0
#define DOOM_KEY_STRAFE_R   0xa1
#define DOOM_KEY_USE        0xa2
#define DOOM_KEY_FIRE       0xa3
#define DOOM_KEY_ESCAPE     27
#define DOOM_KEY_ENTER      13
#define DOOM_KEY_TAB        9
#define DOOM_KEY_BACKSPACE  0x7f
#define DOOM_KEY_RSHIFT     (0x80 + 0x36)
#define DOOM_KEY_RALT       (0x80 + 0x38)

// The key that shows or hides the fps label (and restarts its statistics): the backquote key
#define DOOM_CODE_FPS_TOGGLE 0x0b

// The DOOM key for a keyboard code, DOOM_KEY_NONE for a key the game does not use.
uint8_t doom_key_for_code(uint8_t code);

// One producer (the LVGL task's key callback), one consumer (the engine task's DG_GetKey): a ring that never blocks either.
// A press arriving while it is nearly full is dropped, a release only when it is full (the engine drains it every tick, 32
// events are about a second of typing).
#define DOOM_KEYQ_SIZE 32
#define DOOM_KEYQ_RELEASE_ROOM 8 // slots only a release may take
typedef struct {
    uint16_t       ev[DOOM_KEYQ_SIZE]; // pressed << 8 | key
    volatile uint32_t head;            // written by the producer
    volatile uint32_t tail;            // written by the consumer
} doom_keyq_t;

void doom_keyq_init(doom_keyq_t *q);
bool doom_keyq_push(doom_keyq_t *q, bool pressed, uint8_t key);
bool doom_keyq_pop(doom_keyq_t *q, bool *pressed, uint8_t *key);

// ---- fps -----------------------------------------------------------------------------------------------------------------
// #526: frames handed to the display are counted per one-second window of lv_tick; the average and the lowest window are
// over the windows that are complete, so the half-second a run is stopped in does not count as a slow second.

typedef struct {
    bool     started;
    uint32_t win_start; // ms of the open window
    uint32_t win_frames;
    uint32_t windows;   // complete windows
    uint32_t frames;    // frames in the complete windows
    uint32_t min_win;   // fewest frames in a complete window
    uint32_t last_win;  // frames in the latest complete window
} doom_fps_t;

void     doom_fps_reset(doom_fps_t *f);
// One frame shown at now_ms (a wrapping millisecond tick).
void     doom_fps_frame(doom_fps_t *f, uint32_t now_ms);
// Closes the windows that ended by now_ms without a frame (a stall counts as 0 fps windows).
void     doom_fps_advance(doom_fps_t *f, uint32_t now_ms);
// Average frames per second times 10 over the complete windows, 0 before the first.
uint32_t doom_fps_avg_x10(doom_fps_t const *f);
uint32_t doom_fps_min(doom_fps_t const *f); // 0 before the first complete window

// ---- WAD loading ---------------------------------------------------------------------------------------------------------
// Percent of the WAD file read so far, never 100 while the engine still loads (the bar ends when the first frame comes).
uint32_t doom_load_percent(uint32_t bytes_read, uint32_t wad_size);

// ---- picture -------------------------------------------------------------------------------------------------------------
#define DOOM_SRC_W 320
#define DOOM_SRC_H 200

// RGB565 of an 8-bit-per-channel colour, as LVGL's RGB565 canvas format holds it (red in the top bits)
uint16_t doom_rgb565(uint8_t r, uint8_t g, uint8_t b);
// The 320x200 8-bit frame (palette indexes) as a 640x400 RGB565 picture, every pixel twice across and down. dst (640x400
// RGB565, no padding between lines) must be 4-byte aligned; pal is the 256 RGB565 colours.
void     doom_expand_2x(void *dst, uint8_t const *src, uint16_t const pal[256]);
// The same for an I8 canvas: palette indexes stay indexes, 640x400 bytes with no padding between lines (the palette is LVGL's,
// 1 KB before the pixels). dst must be 4-byte aligned.
void     doom_expand_2x_i8(void *dst, uint8_t const *src);
