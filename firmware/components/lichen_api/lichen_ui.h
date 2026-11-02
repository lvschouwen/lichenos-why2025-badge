#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// The shell's navigation for apps (#84): open a screen, go back, scroll. Implemented by the shell (lichen_shell's
// shell.c and app_settings.c), resolved at link time; lichen_ui_call and lichen_ui_with_group are lichen_api's own
// (lichen_ui.c). All functions take the LVGL port lock themselves and may be called
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
// Turns off LVGL's own arrow-key scrolling for an object that takes the keys itself (shell_page_create, app screens
// and screens without focusable widgets have it off already): call it on a scrollable object an app adds to its group
void shell_no_arrow_scroll(lv_obj_t *obj);
// Pop one screen / down to `screen` (nothing is popped when it is not on the stack), then call then(arg), all in one
// async step: the target is showing when then runs, whatever else is queued (LVGL runs async calls newest first).
// False when the step could not be queued (no memory, the LVGL lock busy): then will not run and arg is still the
// caller's.
bool shell_back_then(void (*then)(void *), void *arg);
bool shell_back_to(lv_obj_t *screen, void (*then)(void *), void *arg);
// The screen from_top places below the top of the shell's stack (0: the top one, 1: the one under it), NULL past the
// bottom. The stack, not what is drawn: a screensaver or lock cover over it does not count (#126).
lv_obj_t *shell_stack_screen(int from_top);
// The shell's screen style (background, text colour, the small font), as shell_page_create uses it; text_entry.c's
// screen too
void shell_style_screen(lv_obj_t *screen);

// A warning or confirm dialog over every screen (#14 T-B; apps too: a careful Remove, #49), e.g. "C6 (radio/Wi-Fi)
// not responding". The message is "<title> <lead>(cancels in N s)": the dialog writes the countdown itself, in that one
// form (#135, R8/R9). A careful dialog says "cancels", any other "(<confirm_label> in N s)" or "(confirms in N s)". lead
// is whatever count_prefix holds before its last '(' (a sentence of your own ahead of the countdown); count_prefix and
// count_suffix are otherwise unused now, leave them NULL. The hint below is dim, amber only when careful. Enter confirms,
// Esc cancels, and when the countdown runs out it confirms by itself (the keyboard may be dead), or cancels when careful.
// done runs once on the LVGL task with the answer: keep it short (queue the work). One dialog at a time: a second
// call replaces the first. Safe from any task, never blocks on more than the LVGL lock (2 s); false when it could not
// be shown.
typedef struct {
    char const *title;
    char const *count_prefix;
    char const *count_suffix;
    char const *hint;          // NULL or "": generated from confirm_label and cancel_label
    uint32_t    seconds;
    bool        careful; // the countdown ends in a cancel and Enter counts only after a second (an erase, #99)
    char const *confirm_label; // the action as a verb ("restart"): the hint reads "Enter: restart • Esc: cancel"
    char const *cancel_label;  // NULL: "cancel"
} shell_dialog_t;
typedef void (*shell_dialog_fn)(bool confirmed);
bool shell_dialog_countdown(shell_dialog_t const *d, shell_dialog_fn done);
// Closes it without an answer (the reason for it went away); safe from any task
void shell_dialog_close(void);

// Runs fn(arg) on the LVGL task, soon (lv_async_call under the LVGL lock): the way a job on any other task hands its
// result back to a screen (#84 T8a). Safe from any task, the LVGL task included. fn runs in LVGL context and may touch
// widgets; the screen it meant may be gone by then, so it checks before it does. False when the call was not queued
// (the LVGL lock was not free within 2 s, or no memory): fn will not run and arg is still the caller's, so retry or
// free it. Never take lvgl_port_lock or call lv_async_call in an app.
bool lichen_ui_call(void (*fn)(void *), void *arg);
// LVGL context only: runs build(arg) with group as the default group, so the widgets it creates join group (rows added
// to a screen that is already showing), then puts the previous default back. Never set the default group yourself.
void lichen_ui_with_group(lv_group_t *group, void (*build)(void *), void *arg);

// The pocket lock (#75) is up: the lock screen covers every screen and no key reaches an app until it is unlocked. An app
// that would act on its own (open a screen for an incoming message) may wait; screens it opens meanwhile stay under the
// lock. Any task.
bool lichen_screen_locked(void);

// Labels for text whose length the code does not control (names, file names, values from a device, versions): a plain
// lv_label grows to fit its text and its parent clips it without a "..." (#131, #133). Use these for any such text;
// `fw ui overflow` lists labels that still do not fit. LVGL context only. width: LV_PCT(n), pixels or LV_SIZE_CONTENT
// with a max_width set by the caller.
// One line, ends in "..." when it does not fit; the height is one line of its font plus its padding and border as set
// now, so call lichen_label_line_fit after changing the font, padding or border
lv_obj_t *lichen_label_line(lv_obj_t *parent, int32_t width);
void      lichen_label_line_fit(lv_obj_t *label);
// Wraps onto as many lines as it needs
lv_obj_t *lichen_label_wrap(lv_obj_t *parent, int32_t width);

// Shared page parts (#135; the UI standard's R1 and R6). Apps use these rather than building their own.
// A page heading that names its subject (R1): a small dim kind line ("Repeater settings") over the name in LARGE on its
// own line, wrapping to two lines and ending in "..." only past that. kind NULL or "": the name alone, still wrapping.
// Never fold both into one "Kind: name" line. One container, so a menu keeps it as its first child. LVGL context only.
lv_obj_t *lichen_heading(lv_obj_t *parent, char const *kind, char const *name);
// A hint line pinned to the bottom of a screen (R6), dim, out of the scroll flow: "Enter: open \xE2\x80\xA2 Del: delete"
// from key/verb pairs, NULL-terminated: shell_page_hint(screen, "Enter", "open", "Del", "delete", NULL). Leaves the
// room at the screen's bottom for it (pad_bottom), so build it before or after the content. Calling it again replaces
// the screen's hint (rebuilt pages call it on every rebuild). No "Esc: back" pairs: Esc
// goes back everywhere. Calling it again on the same screen replaces the text. Returns the label. LVGL context only.
lv_obj_t *shell_page_hint(lv_obj_t *screen, ...);
