#pragma once

// TCA8418 key codes (1-0x50, without the press bit) of the WHY2025 badge keyboard as BadgeVMS scancodes (key_codes.h); index is
// code - 1. From BadgeVMS. Pure data: also built into the host tests.

#include "key_codes.h"

static keyboard_scancode_t const tca8418_keymap[80] = {
    KEY_SCANCODE_ESCAPE,    // 0x1
    KEY_SCANCODE_SQUARE,    // 0x2
    KEY_SCANCODE_TRIANGLE,  // 0x3
    KEY_SCANCODE_CROSS,     // 0x4
    KEY_SCANCODE_CIRCLE,    // 0x5
    KEY_SCANCODE_CLOUD,     // 0x6
    KEY_SCANCODE_DIAMOND,   // 0x7
    KEY_SCANCODE_BACKSPACE, // 0x8
    KEY_SCANCODE_0,         // 0x9
    KEY_SCANCODE_MINUS,     // 0xa
    KEY_SCANCODE_GRAVE,     // 0xb
    KEY_SCANCODE_1,         // 0xc
    KEY_SCANCODE_2,         // 0xd
    KEY_SCANCODE_3,         // 0xe
    KEY_SCANCODE_4,         // 0xf

    KEY_SCANCODE_5,   // 0x10
    KEY_SCANCODE_6,   // 0x11
    KEY_SCANCODE_7,   // 0x12
    KEY_SCANCODE_8,   // 0x13
    KEY_SCANCODE_9,   // 0x14
    KEY_SCANCODE_TAB, // 0x15
    KEY_SCANCODE_Q,   // 0x16
    KEY_SCANCODE_W,   // 0x17
    KEY_SCANCODE_E,   // 0x18
    KEY_SCANCODE_R,   // 0x19
    KEY_SCANCODE_T,   // 0x1a
    KEY_SCANCODE_Y,   // 0x1b
    KEY_SCANCODE_U,   // 0x1c
    KEY_SCANCODE_I,   // 0x1d
    KEY_SCANCODE_O,   // 0x1e
    KEY_SCANCODE_FN,  // 0x1f

    KEY_SCANCODE_A,      // 0x20
    KEY_SCANCODE_S,      // 0x21
    KEY_SCANCODE_D,      // 0x22
    KEY_SCANCODE_F,      // 0x23
    KEY_SCANCODE_G,      // 0x24
    KEY_SCANCODE_H,      // 0x25
    KEY_SCANCODE_J,      // 0x26
    KEY_SCANCODE_K,      // 0x27
    KEY_SCANCODE_L,      // 0x28
    KEY_SCANCODE_LSHIFT, // 0x29
    KEY_SCANCODE_Z,      // 0x2a
    KEY_SCANCODE_X,      // 0x2b
    KEY_SCANCODE_C,      // 0x2c
    KEY_SCANCODE_V,      // 0x2d
    KEY_SCANCODE_B,      // 0x2e
    KEY_SCANCODE_N,      // 0x2f

    KEY_SCANCODE_M,          // 0x30
    KEY_SCANCODE_COMMA,      // 0x31
    KEY_SCANCODE_PERIOD,     // 0x32
    KEY_SCANCODE_LEFT,       // 0x33
    KEY_SCANCODE_DOWN,       // 0x34
    KEY_SCANCODE_RIGHT,      // 0x35
    KEY_SCANCODE_SLASH,      // 0x36
    KEY_SCANCODE_UP,         // 0x37
    KEY_SCANCODE_RSHIFT,     // 0x38
    KEY_SCANCODE_SEMICOLON,  // 0x39
    KEY_SCANCODE_APOSTROPHE, // 0x3a
    KEY_SCANCODE_RETURN,     // 0x3b
    KEY_SCANCODE_EQUALS,     // 0x3c
    KEY_SCANCODE_LCTRL,      // 0x3d
    KEY_SCANCODE_LGUI,       // 0x3e
    KEY_SCANCODE_LALT,       // 0x3f

    KEY_SCANCODE_BACKSLASH,   // 0x40
    KEY_SCANCODE_SPACE,       // 0x41
    KEY_SCANCODE_SPACE,       // 0x42
    KEY_SCANCODE_SPACE,       // 0x43
    KEY_SCANCODE_RALT,        // 0x44
    KEY_SCANCODE_P,           // 0x45
    KEY_SCANCODE_LEFTBRACKET, // 0x46
    KEY_SCANCODE_UNKNOWN,     // 0x47
    KEY_SCANCODE_UNKNOWN,     // 0x48
    KEY_SCANCODE_UNKNOWN,     // 0x49
    KEY_SCANCODE_UNKNOWN,     // 0x4a
    KEY_SCANCODE_UNKNOWN,     // 0x4b
    KEY_SCANCODE_UNKNOWN,     // 0x4c
    KEY_SCANCODE_UNKNOWN,     // 0x4d
    KEY_SCANCODE_UNKNOWN,     // 0x4e
    KEY_SCANCODE_UNKNOWN,     // 0x4f

    KEY_SCANCODE_RIGHTBRACKET, // 0x50
};
