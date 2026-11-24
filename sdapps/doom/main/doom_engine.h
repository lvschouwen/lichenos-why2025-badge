#pragma once
// What the app uses of the engine (third_party/doomgeneric/doomgeneric/doomgeneric.h, built with CMAP256: an 8-bit frame
// of 320x200 plus the palette) and what the engine wants from the backend. Declared here because an app's own sources
// may include only its own headers, lichen_api and system headers (tools/check_sdapp_boundary.py).
#include <stdint.h>

#define DOOMGENERIC_RESX 320
#define DOOMGENERIC_RESY 200

extern uint8_t *DG_ScreenBuffer; // allocated by the engine at RESX*RESY*4

// The palette (CMAP256, i_video.c): set by the engine whenever it changes, one entry per index
struct doom_color {
    uint32_t b : 8;
    uint32_t g : 8;
    uint32_t r : 8;
    uint32_t a : 8;
};
extern struct doom_color colors[256];
extern int               palette_changed; // the engine's `boolean`, an int-sized type

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

void     DG_Init(void);
void     DG_DrawFrame(void);
void     DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs(void);
int      DG_GetKey(int *pressed, unsigned char *key);
void     DG_SetWindowTitle(char const *title);
