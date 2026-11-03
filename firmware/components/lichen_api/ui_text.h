#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The text and choices behind Lichen's shared UI widgets (#135), pure so host_tests/ui_text_test.c covers them: page
// hints, status lines, empty states, the dialog's countdown and hint, and where the focus lands after a refresh.
// Every builder writes at most n bytes (always NUL-terminated for n > 0) and returns the length written.

// Page hint "Enter: open • Del: delete" (R6) from key/verb pairs. A pair with a NULL verb ends the list.
size_t ui_hint_build(char *out, size_t n, ...); // key, verb, key, verb, ..., NULL
size_t ui_hint_vbuild(char *out, size_t n, va_list ap);

// The bottom padding a screen needs so its content ends above the pinned hint (#142): the hint's line, the gap the hint
// keeps off the screen edge, and a gap between content and hint. keep is the padding the screen already has: the result
// is never smaller, and calling it again with its own result changes nothing (a rebuilt screen does not grow).
int32_t ui_hint_pad(int32_t keep, int32_t line, int32_t edge, int32_t gap);

// Dialog hint from the labels of a confirm: "Enter: restart • Esc: cancel" (cancel_label NULL: "cancel")
size_t ui_confirm_hint(char *out, size_t n, char const *confirm_label, char const *cancel_label);

// The dialog's message: "<title> <lead>(cancels in N s)" in one form (R8/R9). lead is the text of prefix before its last
// '(' (a caller's own sentence ahead of the countdown), "" when the prefix has none. A careful dialog's countdown
// cancels; any other confirms by itself, and then names the action ("(restart in N s)") when confirm_label is given.
size_t ui_countdown_text(char *out, size_t n, char const *title, char const *prefix, bool careful,
                         char const *confirm_label, uint32_t seconds);

// A status line that ends (R5): begin shows "<verb>...", progress "<verb> 3 of 17", done the final text, fail "<why>.
// <next>" in plain words (the full stop is added when missing, next may be NULL).
typedef enum { UI_STATUS_IDLE, UI_STATUS_RUNNING, UI_STATUS_DONE, UI_STATUS_FAILED } ui_status_state_t;
typedef struct {
    ui_status_state_t state;
    char              verb[32];
} ui_status_t;
// Starts a run. True when the previous run was never ended (done or fail): the caller logs it.
bool   ui_status_begin(ui_status_t *s, char const *verb);
// Ends a run. False when none was running.
bool   ui_status_end(ui_status_t *s, bool failed);
size_t ui_status_begin_text(char *out, size_t n, char const *verb);
size_t ui_status_progress_text(char *out, size_t n, char const *verb, int done, int of); // of <= 0: "<verb> 3"
size_t ui_status_fail_text(char *out, size_t n, char const *why, char const *next);

// An empty list: "No contacts yet: they appear when a node adverts." (how NULL: "No contacts yet.") (R11)
size_t ui_empty_text(char *out, size_t n, char const *what, char const *how);
