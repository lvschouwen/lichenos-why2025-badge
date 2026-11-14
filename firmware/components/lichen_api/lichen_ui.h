#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The shell's navigation for apps (#84): open a screen, go back, scroll. The shell does the work: the gate forwards
// these calls to the table the shell sets at boot (lichen_ui_ops.h, #221), and until then each has a defined "not up"
// answer (nothing, false, NULL, 0). shell_key_hash is the gate's own; lichen_ui_call and lichen_ui_with_group are the UI
// service's (lichen_os/lichen_ui_call.c, #220, #225: the only owner of the LVGL lock and the default group; the shell
// takes the lock through lichen_sys_ui.h), the label and heading helpers the gate's own (lichen_ui.c).
//
// Thread rule (#238), stated in each function's comment and copied into tools/gate_manifest.txt: "any task" = the
// function takes the LVGL port lock itself (or touches no LVGL state), so any task may call it; "LVGL task only" = it
// calls LVGL directly without the lock, so only the LVGL task (a builder, a key or timer callback, a lichen_ui_call
// callback) may. Unmarked is not "any": a new function states its rule.

// Any task (takes the lock).
void shell_home(void);
// Like Esc: pops the top screen, later (outside key handling). For a screen that takes Esc itself (the Keyboard test).
// Any task (takes the lock).
void shell_back(void);
// Builds a screen (lv_obj_create(NULL), runs in LVGL context) and shows it on top of the stack; Esc goes back. The
// builder's focusable widgets are put in a group of their own (the default group while it runs). Any task (takes the
// lock; the builder itself runs in LVGL context).
//
// One screen per identity (#148): a screen is (screen_id, key), shell_open's key is 0. Opening an identity that is
// already on the stack never builds a second copy: on top it does nothing (a repeated Enter), lower down the shell goes
// back to it (as shell_back_to; refused while a dialog is open). So a screen may keep its state in file statics.
void shell_open(lv_obj_t *(*build)(void), int screen_id);
// shell_open for a screen id that shows one of many things (a chat, a contact, a file): key says which one, e.g.
// shell_key_hash() of what identifies it. Different keys stack as separate screens, the same key is the same screen.
// SHELL_KEY_UNIQUE: always a new screen, never matched (state must then live in the screen, not in statics). Any task
// (takes the lock; the builder itself runs in LVGL context).
#define SHELL_KEY_UNIQUE 0xFFFFFFFFu
void shell_open_keyed(lv_obj_t *(*build)(void), int screen_id, uint32_t key);
// A key from bytes (a contact's key prefix, a path, a chat key); never SHELL_KEY_UNIQUE. Any task (pure, no LVGL state).
uint32_t shell_key_hash(void const *data, size_t len);
// How many shell_open calls found their screen already open (on top, or went back to it); an opener that marks
// something done only when a screen really opened compares it before and after (notify_centre.c). Any task (a plain
// read of a counter, no LVGL state).
unsigned shell_open_reuse_count(void);
// An opener that shows its screen another way (meshcore_chat.c: it replaces the chat) counts it as a reuse. LVGL task
// only (the counter is not locked).
void shell_open_count_reuse(void);
// Whether (screen_id, key) is on the stack now. Check it before writing a screen's file statics for a shell_open that
// would only reuse the screen (and so not run the builder that reads them). Any task (takes the lock).
bool shell_screen_open(int screen_id, uint32_t key);
// Called from inside a builder: the screen gets first use of Esc. fn(arg) runs on Esc (LVGL context); when it returns
// true the Esc is used up (a search or a draft was cleared), otherwise the screen goes back as usual (#32). LVGL task
// only (inside a builder).
void shell_on_esc(bool (*fn)(void *arg), void *arg);
// Called from inside a builder, like shell_on_esc: the screen can search (a list filter, a chat). fn(arg) runs on the
// search key (●, #79; LVGL context) and starts or restarts the screen's search. Screens without one ignore the key.
// LVGL task only (inside a builder).
void shell_on_search(void (*fn)(void *arg), void *arg);
// Up/Down on a scrollable object: moves by dy within its content only, so nothing scrolls when it all fits and nothing
// overscrolls. LVGL task only.
void shell_scroll_by(lv_obj_t *obj, int32_t dy);
// A new screen styled as Settings' pages (free room for the status bar, the screen edge), for an app's own pages
// opened with shell_open(); put a menu_create() menu on it. LVGL task only.
lv_obj_t *shell_page_create(void);
// Turns off LVGL's elastic and momentum scrolling on obj (screens, menus, lists). LVGL task only.
void shell_scroll_tame(lv_obj_t *obj);
// Turns off LVGL's own arrow-key scrolling for an object that takes the keys itself (shell_page_create, app screens
// and screens without focusable widgets have it off already): call it on a scrollable object an app adds to its group.
// LVGL task only.
void shell_no_arrow_scroll(lv_obj_t *obj);
// Pop one screen / down to `screen` (nothing is popped when it is not on the stack), then call then(arg), all in one
// async step: the target is showing when then runs, whatever else is queued (LVGL runs async calls newest first).
// False when the step could not be queued (no memory, the LVGL lock busy): then will not run and arg is still the
// caller's. Any task (takes the lock).
bool shell_back_then(void (*then)(void *), void *arg);
// Like shell_back_then, but down to `screen`. Any task (takes the lock).
bool shell_back_to(lv_obj_t *screen, void (*then)(void *), void *arg);
// The screen from_top places below the top of the shell's stack (0: the top one, 1: the one under it), NULL past the
// bottom. The stack, not what is drawn: a screensaver or lock cover over it does not count (#126). Any task (takes the
// lock; use the screen only on the LVGL task).
lv_obj_t *shell_stack_screen(int from_top);
// The shell's screen style (background, text colour, the small font), as shell_page_create uses it; text_entry.c's
// screen too. LVGL task only.
void shell_style_screen(lv_obj_t *screen);

// A warning or confirm dialog over every screen (#14 T-B; apps too: a careful Remove, #49), e.g. "C6 (radio/Wi-Fi)
// not responding". The message is "<title> <lead>(cancels in N s)": the dialog writes the countdown itself, in that one
// form (#135, R8/R9). A careful dialog says "cancels", any other "(<confirm_label> in N s)" or "(confirms in N s)". lead
// is whatever count_prefix holds before its last '(' (a sentence of your own ahead of the countdown); count_prefix and
// count_suffix are otherwise unused now, leave them NULL. The hint below is dim, amber only when careful. Enter confirms,
// Esc cancels, and when the countdown runs out it confirms by itself (the keyboard may be dead), or cancels when careful.
// done runs once on the LVGL task with the answer: keep it short (queue the work). One dialog at a time: a second
// call replaces the first. Any task (waits at most 2 s for another task's open, then 2 s for the LVGL lock); false
// when it could not be shown. The strings are copied, from internal RAM or PSRAM alike.
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
// Shows the dialog described by d. Any task (takes the lock, see above).
bool shell_dialog_countdown(shell_dialog_t const *d, shell_dialog_fn done);
// Closes it without an answer (the reason for it went away). Any task.
void shell_dialog_close(void);

// Runs fn(arg) on the LVGL task, soon (the UI service's queue, drained on the next LVGL timer pass; FIFO with every other
// deferred call of the OS unless a burst overflows the queue, #286): the way a job on any other task hands its result
// back to a screen (#84 T8a, #225). Any task (takes the lock), the LVGL task included; never inline, also from the LVGL task. fn runs
// in LVGL context and may touch widgets; the screen it meant may be gone by then, so it checks before it does. False when
// the call was not queued (the LVGL lock was not free within 2 s, or no memory): fn will not run and arg is still the
// caller's, so retry or free it. Never take lvgl_port_lock or call lv_async_call in an app.
bool lichen_ui_call(void (*fn)(void *), void *arg);
// LVGL task only: runs build(arg) with group as the default group, so the widgets it creates join group (rows added
// to a screen that is already showing), then puts the previous default back. Never set the default group yourself.
void lichen_ui_with_group(lv_group_t *group, void (*build)(void *), void *arg);

// lv_obj_add_event_cb and lv_timer_create for an app (#349; tools/layers.conf keeps the raw two from the apps): cb runs
// as the context that registered it (lichen_ctx_self() now: the app inside its hooks, its callbacks and its UI calls),
// so its memory, tasks, capability checks and a crash in it count for that app, not for the OS. Same arguments and
// answers as LVGL's (user_data stays what lv_event_get_user_data / lv_timer_get_user_data return; NULL when out of
// memory). The record (8 bytes, LVGL's heap in PSRAM) is freed with the descriptor or the timer: removing the event,
// deleting the object or the timer, a one-shot timer ending. Remove such an event by descriptor
// (lv_obj_remove_event_dsc), not by cb. LVGL task only (a builder or a callback); takes no lock.
lv_event_dsc_t *lichen_ui_event_add(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t filter, void *user_data);
// lv_timer_create the same way: the timer's cb runs as the registering context (never lv_timer_set_cb on it, which
// would drop the trampoline; no lv_timer_create_basic either). LVGL task only; takes no lock.
lv_timer_t     *lichen_ui_timer_create(lv_timer_cb_t cb, uint32_t period_ms, void *user_data);
// lv_group_set_focus_cb the same way, for a screen's own group (shell_open's): cb runs as the registering context.
// NULL takes the callback away. The record is freed with the group or by the next call. LVGL task only; no lock.
void            lichen_ui_group_focus_cb(lv_group_t *group, lv_group_focus_cb_t cb);

// The pocket lock (#75) is up: the lock screen covers every screen and no key reaches an app until it is unlocked. An app
// that would act on its own (open a screen for an incoming message) may wait; screens it opens meanwhile stay under the
// lock. Any task (an atomic read).
bool lichen_screen_locked(void);

// The Swiss railway style station clock (#122), the screensaver's dial as an app's view: fills screen's content area
// with the dial, the date under it and a small dim second line that update_line2(line2) sets once a second (NULL: left
// empty). Its own timer and PSRAM canvas, freed with the screen; the sweep pauses while another screen covers it. False
// when there is no PSRAM for the dial (nothing was added), or before the shell is up: show the time another way. The
// shell's (station_view.c). LVGL task only.
bool lichen_station_clock(lv_obj_t *screen, void (*update_line2)(lv_obj_t *line2));
// The date line as the station clock writes it ("~ " first while the time is unverified); false and "Clock not set"
// when the clock is not set, or false and "" before the shell is up. Any task (no LVGL state).
bool lichen_date_text(char *out, size_t cap);

// A filled box for a home icon (lichen_app_t.draw_icon): w x h, centred at (x, y) off the parent's centre, in colour
// (0xRRGGBB) with radius (LV_RADIUS_CIRCLE for a dot). LVGL task only.
lv_obj_t *lichen_icon_box(lv_obj_t *parent, int32_t w, int32_t h, int32_t x, int32_t y, uint32_t color, int32_t radius);

// Labels for text whose length the code does not control (names, file names, values from a device, versions): a plain
// lv_label grows to fit its text and its parent clips it without a "..." (#131, #133). Use these for any such text;
// `fw ui overflow` lists labels that still do not fit. All of these are LVGL task only. width: LV_PCT(n), pixels or LV_SIZE_CONTENT
// with a max_width set by the caller.
// One line, ends in "..." when it does not fit; the height is one line of its font plus its padding and border as set
// now, so call lichen_label_line_fit after changing the font, padding or border. LVGL task only.
lv_obj_t *lichen_label_line(lv_obj_t *parent, int32_t width);
// Re-fits a lichen_label_line label's height after a font, padding or border change. LVGL task only.
void      lichen_label_line_fit(lv_obj_t *label);
// Wraps onto as many lines as it needs. LVGL task only.
lv_obj_t *lichen_label_wrap(lv_obj_t *parent, int32_t width);

// Shared page parts (#135; the UI standard's R1 and R6). Apps use these rather than building their own.
// A page heading that names its subject (R1): a small dim kind line ("Repeater settings") over the name in LARGE on its
// own line, wrapping to two lines and ending in "..." only past that. kind NULL or "": the name alone, still wrapping.
// Never fold both into one "Kind: name" line. One container, so a menu keeps it as its first child. LVGL task only.
lv_obj_t *lichen_heading(lv_obj_t *parent, char const *kind, char const *name);
// A hint line pinned to the bottom of a screen (R6), dim, out of the scroll flow: "Enter: open \xE2\x80\xA2 Del: delete"
// from key/verb pairs, NULL-terminated: shell_page_hint(screen, "Enter", "open", "Del", "delete", NULL). Leaves the
// room at the screen's bottom for it (pad_bottom), so build it before or after the content. Calling it again replaces
// the screen's hint (rebuilt pages call it on every rebuild). No "Esc: back" pairs: Esc
// goes back everywhere. Calling it again on the same screen replaces the text. Returns the label. LVGL task only.
lv_obj_t *shell_page_hint(lv_obj_t *screen, ...);
// The hint shell_page_hint put on `screen`, NULL if it has none. For `fw ui overflow` (#133), which checks that no
// content runs under it. LVGL task only.
lv_obj_t *lichen_page_hint_of(lv_obj_t *screen);
