#pragma once

// Lichen's colours and sizes (#66, design "Brand"): the existing UI palette plus teal for the C6's half of the logo

#define THEME_BG     0x101820
#define THEME_TEXT   0xF2F2F2
#define THEME_DIM    0x8090A0
#define THEME_AMBER  0xF5B700
#define THEME_TEAL   0x3FC1B0
#define THEME_RED    0xE5534B // a failed boot step
#define THEME_LINK_RED 0xE5484D // a down link in the status bar (brighter than a failed boot step)
#define THEME_TILE   0x16212B // an unselected home tile
#define THEME_TRACK  0x1F2B36 // empty bar segments, selected tiles
#define THEME_RULE   0x1C2733 // the line under the status bar
#define THEME_FAINT  0x2F3D4B // skipped boot steps

#define THEME_PAD          36 // screen edge on the 720x720 panel (the mockups' 18 px at half size)
#define THEME_STATUS_BAR_H 56 // screens opened with shell_open() keep this much free at the top
