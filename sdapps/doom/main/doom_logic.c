// The pure half of the DOOM app's video and keys (#526), see doom_logic.h. GPL-3.0-or-later, like the app as a whole.
#include "doom_logic.h"

#include <string.h>

// ---- keys ----------------------------------------------------------------------------------------------------------------

uint8_t doom_key_for_code(uint8_t code) {
    switch (code) {
    case 0x01: return DOOM_KEY_ESCAPE;     // the DOOM menu; Quit there ends the app
    case 0x3b: return DOOM_KEY_ENTER;
    case 0x15: return DOOM_KEY_TAB;        // the map
    case 0x08: return DOOM_KEY_BACKSPACE;
    case 0x37: case 0x17: return DOOM_KEY_UPARROW;    // up, W
    case 0x34: case 0x21: return DOOM_KEY_DOWNARROW;  // down, S
    case 0x33: return DOOM_KEY_LEFTARROW;
    case 0x35: return DOOM_KEY_RIGHTARROW;
    case 0x20: return DOOM_KEY_STRAFE_L;   // A
    case 0x22: return DOOM_KEY_STRAFE_R;   // D
    case 0x3d: case 0x23: return DOOM_KEY_FIRE;       // Ctrl, F
    case 0x41: case 0x42: case 0x43: case 0x18: return DOOM_KEY_USE; // the space bar, E
    case 0x29: case 0x38: return DOOM_KEY_RSHIFT;     // run
    case 0x3f: case 0x44: return DOOM_KEY_RALT;       // strafe while held
    case 0x0c: return '1';
    case 0x0d: return '2';
    case 0x0e: return '3';
    case 0x0f: return '4';
    case 0x10: return '5';
    case 0x11: return '6';
    case 0x12: return '7';
    case 0x1b: return 'y'; // the menu's "press y to quit"
    case 0x2f: return 'n';
    case 0x0a: return '-';
    case 0x3c: return '=';
    default: return DOOM_KEY_NONE;
    }
}

void doom_keyq_init(doom_keyq_t *q) {
    memset(q, 0, sizeof(*q));
}

bool doom_keyq_push(doom_keyq_t *q, bool pressed, uint8_t key) {
    uint32_t head = q->head;
    if (head - q->tail >= DOOM_KEYQ_SIZE) {
        return false;
    }
    q->ev[head % DOOM_KEYQ_SIZE] = (uint16_t)((pressed ? 0x100 : 0) | key);
    __atomic_store_n(&q->head, head + 1, __ATOMIC_RELEASE); // the event is written before the consumer can see it
    return true;
}

bool doom_keyq_pop(doom_keyq_t *q, bool *pressed, uint8_t *key) {
    uint32_t tail = q->tail;
    if (tail == __atomic_load_n(&q->head, __ATOMIC_ACQUIRE)) {
        return false;
    }
    uint16_t e = q->ev[tail % DOOM_KEYQ_SIZE];
    *pressed   = (e & 0x100) != 0;
    *key       = (uint8_t)(e & 0xff);
    __atomic_store_n(&q->tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

// ---- fps -----------------------------------------------------------------------------------------------------------------

void doom_fps_reset(doom_fps_t *f) {
    memset(f, 0, sizeof(*f));
}

static void close_window(doom_fps_t *f) {
    if (f->windows == 0 || f->win_frames < f->min_win) {
        f->min_win = f->win_frames;
    }
    f->windows++;
    f->frames += f->win_frames;
    f->last_win   = f->win_frames;
    f->win_frames = 0;
    f->win_start += 1000;
}

void doom_fps_advance(doom_fps_t *f, uint32_t now_ms) {
    if (!f->started) {
        return;
    }
    // a stall of minutes would loop for minutes of windows: past an hour the numbers are no use anyway
    for (int guard = 0; guard < 3600 && (uint32_t)(now_ms - f->win_start) >= 1000; guard++) {
        close_window(f);
    }
}

void doom_fps_frame(doom_fps_t *f, uint32_t now_ms) {
    if (!f->started) {
        f->started   = true;
        f->win_start = now_ms;
    }
    doom_fps_advance(f, now_ms);
    f->win_frames++;
}

uint32_t doom_fps_avg_x10(doom_fps_t const *f) {
    return f->windows ? (f->frames * 10 + f->windows / 2) / f->windows : 0;
}

uint32_t doom_fps_min(doom_fps_t const *f) {
    return f->windows ? f->min_win : 0;
}

// ---- WAD loading ---------------------------------------------------------------------------------------------------------

uint32_t doom_load_percent(uint32_t bytes_read, uint32_t wad_size) {
    if (wad_size == 0) {
        return 0;
    }
    uint64_t pct = (uint64_t)bytes_read * 100 / wad_size;
    return pct > 99 ? 99 : (uint32_t)pct;
}

// ---- picture -------------------------------------------------------------------------------------------------------------

uint16_t doom_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void doom_expand_2x(void *dst, uint8_t const *src, uint16_t const pal[256]) {
    for (int y = 0; y < DOOM_SRC_H; y++) {
        uint32_t *row = (uint32_t *)dst + (size_t)y * 2 * DOOM_SRC_W; // 320 words a line of 640 pixels
        for (int x = 0; x < DOOM_SRC_W; x++) {
            uint32_t c = pal[src[x]];
            row[x]     = c | (c << 16); // two pixels of the same colour in one 32-bit store
        }
        memcpy(row + DOOM_SRC_W, row, DOOM_SRC_W * 4); // the second line of the pair
        src += DOOM_SRC_W;
    }
}

void doom_expand_2x_i8(void *dst, uint8_t const *src) {
    uint32_t *row = dst;
    for (int y = 0; y < DOOM_SRC_H; y++) {
        for (int x = 0; x < DOOM_SRC_W; x += 2) {
            uint32_t a = src[x], b = src[x + 1];
            row[x / 2] = a | (a << 8) | (b << 16) | (b << 24);
        }
        memcpy(row + DOOM_SRC_W / 2, row, DOOM_SRC_W * 2); // the second line of the pair
        row += DOOM_SRC_W;                                 // two lines of 640 bytes = 320 words
        src += DOOM_SRC_W;
    }
}
