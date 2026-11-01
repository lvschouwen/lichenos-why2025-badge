#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// The shell's navigation for apps (#84): open a screen, go back, scroll. Implemented by the shell (lichen_shell's
// shell.c and app_settings.c), resolved at link time. All functions take the LVGL port lock themselves and may be called
// from any task.

void shell_home(void);
// Builds a screen (lv_obj_create(NULL), runs in LVGL context) and shows it on top of the stack; Esc goes back. The
// builder's focusable widgets are put in a group of their own (the default group while it runs).
void shell_open(lv_obj_t *(*build)(void), int screen_id);
// Called from inside a builder: the screen gets first use of Esc. fn(arg) runs on Esc (LVGL context); when it returns
// true the Esc is used up (a search or a draft was cleared), otherwise the screen goes back as usual (#32).
void shell_on_esc(bool (*fn)(void *arg), void *arg);
// Called from inside a builder, like shell_on_esc: the screen can search (a list filter, a chat). fn(arg) runs on the
// search key (●, #79; LVGL context) and starts or restarts the screen's search. Screens without one ignore the key.
void shell_on_search(void (*fn)(void *arg), void *arg);
// Up/Down on a scrollable object: moves by dy within its content only, so nothing scrolls when it all fits and nothing
// overscrolls. shell_scroll_tame turns off LVGL's elastic and momentum scrolling on obj (screens, menus, lists).
void shell_scroll_by(lv_obj_t *obj, int32_t dy);
// A new screen styled as Settings' pages (free room for the status bar, the screen edge), for an app's own pages
// opened with shell_open(); put a menu_create() menu on it
lv_obj_t *shell_page_create(void);
void shell_scroll_tame(lv_obj_t *obj);
// Pop one screen / down to `screen` (nothing is popped when it is not on the stack), then call then(arg), all in one
// async step: the target is showing when then runs, whatever else is queued (LVGL runs async calls newest first)
void shell_back_then(void (*then)(void *), void *arg);
void shell_back_to(lv_obj_t *screen, void (*then)(void *), void *arg);
// The shell's screen style (background, text colour, the small font), as shell_page_create uses it; text_entry.c's
// screen too
void shell_style_screen(lv_obj_t *screen);

// A warning or confirm dialog over every screen (#14 T-B; apps too: a careful Remove, #49), e.g. "C6 (radio/Wi-Fi)
// not responding — restarting in 30 s". The message reads "<title> <count_prefix><seconds left><count_suffix>", the hint below it. Enter confirms, Esc
// cancels, and when the countdown runs out it confirms by itself (the keyboard may be dead), or cancels when careful.
// done runs once on the LVGL task with the answer: keep it short (queue the work). One dialog at a time: a second
// call replaces the first. Safe from any task, never blocks on more than the LVGL lock (2 s); false when it could not
// be shown.
typedef struct {
    char const *title;
    char const *count_prefix;
    char const *count_suffix;
    char const *hint;
    uint32_t    seconds;
    bool        careful; // the countdown ends in a cancel and Enter counts only after a second (an erase, #99)
} shell_dialog_t;
typedef void (*shell_dialog_fn)(bool confirmed);
bool shell_dialog_countdown(shell_dialog_t const *d, shell_dialog_fn done);
// Closes it without an answer (the reason for it went away); safe from any task
void shell_dialog_close(void);
