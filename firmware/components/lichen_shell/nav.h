#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Which screen is open and where Esc leads (#66). Pure, host-tested in host_tests/nav_test.c; the shell keeps the
// LVGL screens in step with it. Home is always at the bottom and counts towards the depth.

#define NAV_HOME      0
#define NAV_MAX_DEPTH 8

// A screen's identity is (screen id, key) (#148): the key is 0 unless the screen shows one of many things (a chat, a
// contact, a file). NAV_KEY_UNIQUE never matches anything: that screen is always new.
#define NAV_KEY_UNIQUE 0xFFFFFFFFu

typedef enum {
    NAV_OPEN_NEW,     // not on the stack: build it
    NAV_OPEN_KEEP,    // already on top (a repeated Enter): do nothing
    NAV_OPEN_BACK_TO, // lower in the stack: go back to it, never a second copy
} nav_open_t;

void nav_reset(void);
// What opening (screen_id, key) means for the stack as it is. For KEEP and BACK_TO, *frame is the stack position of the
// existing screen (0 = home), else -1.
nav_open_t nav_decide(int screen_id, uint32_t key, int *frame);
// false (nothing changes) when the stack is full or (screen_id, key) is already on it (decide first, then push)
bool nav_push_keyed(int screen_id, uint32_t key);
// nav_push_keyed with key 0
bool nav_push(int screen_id);
// The key at stack position index, 0 past the top
uint32_t nav_key_at(int index);
// Leaves the current screen; returns the one now on top. At home it stays home.
int  nav_back(void);
int  nav_current(void);
int  nav_depth(void);
// The screen id at stack position index (0 = home), -1 past the top (fw ui prints the stack)
int  nav_at(int index);
