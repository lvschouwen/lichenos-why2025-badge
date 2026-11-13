#pragma once

#include "dialog_remote_policy.h"

#include <stdbool.h>
#include <stdint.h>

// What a key sent over the console ("fw keys send", "fw ui answer") may do (#377, decision on #182 and #376): the
// rules behind the stepper's check before every remote Enter, Left or Right and behind "fw ui answer yes <label>".
// Pure, host-tested in host_tests/remote_answer_test.c; ui_answer.c gathers the view and acts on the result.
//   - Enter on a dialog follows its dlg_remote_verdict: REFUSE is hand-only, ALLOW_ONAIR_SCOPED needs the bench
//     console scope (#290), and a dialog that is opening or could not be read is no dialog to answer yet.
//   - The badge's own radio pages are no dialogs (Region & radio with its TX power slider, Custom radio, Switch radio):
//     there Enter, Left and Right need the bench console scope too (Lucas: ALLOW_ONAIR_SCOPED). The whole page counts,
//     not one row: a key reaches the page a few ms after the check, so a row could change under it.
//   - Esc and every other key always go: they only move or go back.

// After this long a careful dialog takes Enter (DLG_CAREFUL_ARM_MS, with a margin): "fw ui answer yes" waits for it
#define REMOTE_ANSWER_ARM_MS 1100u

typedef enum { RKEY_OTHER, RKEY_ENTER, RKEY_SIDE } rkey_t; // SIDE: Left or Right
typedef enum { RDLG_NONE, RDLG_OPEN, RDLG_SETTLING, RDLG_UNREAD } rdlg_t;

typedef enum {
    RANS_OK,
    RANS_HAND_ONLY,    // the dialog's verdict is REFUSE
    RANS_NEEDS_SCOPE,  // a dialog or a radio page that transmits, without the bench console scope
    RANS_UNSURE,       // a dialog is opening, or its state could not be read: ask again shortly
    RANS_NO_DIALOG,    // answer: no dialog is open
    RANS_OTHER_DIALOG, // answer: the open dialog has another confirm label, or is another open (gen)
} rans_t;

typedef struct {
    rdlg_t               dialog;
    dlg_remote_verdict_t verdict;    // of the open dialog
    char const          *label;      // the open dialog's confirm label
    uint32_t             gen;        // its open number (shell_dialog_info_t.gen)
    int                  top_screen; // nav_current(); only the stepper's check reads it
    bool                 scope_set;  // bench_scope_set()
} remote_view_t;

// May a remote key of this kind go down now?
rans_t   remote_key_check(rkey_t key, remote_view_t const *v);
// Is screen_id one of the badge's own radio pages (no dialog: Enter, Left and Right there need the bench scope)?
bool     remote_radio_page(int screen_id);
// "fw ui answer yes <want_label>": may the open dialog be confirmed? An empty or NULL want_label never matches.
rans_t   remote_answer_check(remote_view_t const *v, char const *want_label);
// The same right before its Enter goes down, which must also find the open it started from (want_gen)
rans_t   remote_answer_still(remote_view_t const *v, char const *want_label, uint32_t want_gen);
// How long "fw ui answer yes" waits before its Enter: up to REMOTE_ANSWER_ARM_MS since the dialog opened
uint32_t remote_answer_wait_ms(uint32_t ms_open);
