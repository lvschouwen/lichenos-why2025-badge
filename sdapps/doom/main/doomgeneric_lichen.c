// The Lichen backend of doomgeneric (#525) and the libc the engine gets from doom_shim.h. Video and keys are a stub here
// (#526): the engine runs, draws into DG_ScreenBuffer and nothing shows it. GPL-3.0-or-later, like the app as a whole.
#include "doom_app.h"
#include "doom_engine.h"

#include "lichen_mem.h"
#include "lichen_app.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// this file implements the shim's functions: it needs the real stdio names
#undef fprintf
#undef vfprintf
#undef fputs
#undef fputc
#undef fflush
#undef stdout
#undef stderr
#undef stdin
#undef errno
#undef exit

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lichen_doom_errno;

// ---- counted memory ----------------------------------------------------------------------------------------------------
// A doubly linked list of blocks, each behind a 16-byte header (keeps the block 16-aligned): stop walks it and frees what
// the engine did not (the zone, the screen buffer). Only the engine task allocates, so no lock; stop runs after it ended.

typedef struct blk {
    struct blk *prev;
    struct blk *next;
    size_t      size;
    size_t      pad;
} blk_t;

static blk_t *blocks;
static size_t blocks_bytes;

static void link_blk(blk_t *b) {
    b->prev = NULL;
    b->next = blocks;
    if (blocks) {
        blocks->prev = b;
    }
    blocks = b;
    blocks_bytes += b->size;
}

static void unlink_blk(blk_t *b) {
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        blocks = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    blocks_bytes -= b->size;
}

void *lichen_doom_malloc(size_t n) {
    if (n > SIZE_MAX - sizeof(blk_t)) {
        return NULL; // a lump size read from the card must not wrap into a small block
    }
    blk_t *b = lichen_alloc(lichen_ctx_self(), sizeof(blk_t) + n, LICHEN_MEM_BULK);
    if (!b) {
        return NULL;
    }
    b->size = n;
    link_blk(b);
    return b + 1;
}

void *lichen_doom_calloc(size_t n, size_t size) {
    if (size && n > (SIZE_MAX - sizeof(blk_t)) / size) {
        return NULL;
    }
    void *p = lichen_doom_malloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void lichen_doom_free(void *p) {
    if (!p) {
        return;
    }
    blk_t *b = (blk_t *)p - 1;
    unlink_blk(b);
    lichen_free(lichen_ctx_self(), b);
}

void *lichen_doom_realloc(void *p, size_t n) {
    if (!p) {
        return lichen_doom_malloc(n);
    }
    blk_t *old = (blk_t *)p - 1;
    void  *q   = lichen_doom_malloc(n);
    if (q) {
        memcpy(q, p, old->size < n ? old->size : n);
        lichen_doom_free(p);
    }
    return q;
}

char *lichen_doom_strdup(char const *s) {
    size_t n = strlen(s) + 1;
    char  *p = lichen_doom_malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

void doom_free_all(void) {
    while (blocks) {
        blk_t *b = blocks;
        unlink_blk(b);
        lichen_free(lichen_ctx_self(), b);
    }
    DG_ScreenBuffer = NULL;
}

size_t doom_held_bytes(void) {
    return blocks_bytes;
}

// ---- stdio the OS does not export ---------------------------------------------------------------------------------------
// stdout and stderr are the pointers 1 and 2 (doom_shim.h): written nowhere. Anything else is a handle of the file shims.

static bool is_std(FILE *f) {
    return (uintptr_t)f <= 2u;
}

int lichen_doom_vfprintf(FILE *f, char const *fmt, va_list ap) {
    if (is_std(f)) {
        return 0;
    }
    char buf[256];
    int  n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        return n;
    }
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    return fwrite(buf, 1, len, f) == len ? n : -1;
}

int lichen_doom_fprintf(FILE *f, char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = lichen_doom_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int lichen_doom_fputs(char const *s, FILE *f) {
    if (is_std(f)) {
        return 0;
    }
    size_t len = strlen(s);
    return fwrite(s, 1, len, f) == len ? 0 : -1;
}

int lichen_doom_fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    if (is_std(f)) {
        return c;
    }
    return fwrite(&ch, 1, 1, f) == 1 ? c : -1;
}

int lichen_doom_fflush(FILE *f) {
    return is_std(f) ? 0 : fflush(f);
}

// ---- the doomgeneric backend --------------------------------------------------------------------------------------------

void DG_Init(void) {
}

void DG_DrawFrame(void) {
    // #526: the frame in DG_ScreenBuffer and the palette go to a canvas here
}

void DG_SleepMs(uint32_t ms) {
    uint32_t ticks = ms / portTICK_PERIOD_MS;
    vTaskDelay(ticks ? ticks : 1);
}

uint32_t DG_GetTicksMs(void) {
    return lv_tick_get();
}

int DG_GetKey(int *pressed, unsigned char *key) {
    (void)pressed;
    (void)key;
    return 0; // #526
}

void DG_SetWindowTitle(char const *title) {
    (void)title;
}

// The engine's own quit (the menu's Quit, the end of a demo) comes here: the engine task ends the app. i_system.c's I_Quit
// is renamed lichen_doom_quit_engine in CMakeLists.txt; it runs the engine's exit functions (they only stop its subsystems).
void lichen_doom_quit_engine(void);

void I_Quit(void) {
    lichen_doom_quit_engine();
    lichen_doom_exit(0); // on the engine task: exit() from an LVGL callback would abort the badge
}

void lichen_doom_exit(int code) {
    doom_engine_ending();
    exit(code);
}

void doom_engine_shutdown(void) {
    lichen_doom_quit_engine();
}
