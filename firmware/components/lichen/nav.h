#pragma once

#include <stdbool.h>

// Which screen is open and where Esc leads (#66). Pure, host-tested in host_tests/nav_test.c; the shell keeps the
// LVGL screens in step with it. Home is always at the bottom and counts towards the depth.

#define NAV_HOME      0
#define NAV_MAX_DEPTH 8

void nav_reset(void);
// false (nothing changes) when the stack is full or the screen is already on top (a repeated Enter)
bool nav_push(int screen_id);
// Leaves the current screen; returns the one now on top. At home it stays home.
int  nav_back(void);
int  nav_current(void);
int  nav_depth(void);
// The screen id at stack position index (0 = home), -1 past the top (fw ui prints the stack)
int  nav_at(int index);
