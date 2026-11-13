#pragma once

#include "dialog_remote_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What a key sent over the console ("fw keys send", "fw ui answer") may do (#377, decision on #182 and #376): the
// rules behind the stepper's check before every remote Enter, Left or Right and behind "fw ui answer yes <label>".
// Pure, host-tested in host_tests/remote_answer_test.c; ui_answer.c gathers the view and acts on the result.
//   - Enter on a dialog follows its dlg_remote_verdict: REFUSE is hand-only, ALLOW_ONAIR_SCOPED needs the bench
//     console scope (#290), and a dialog that is opening or could not be read is no dialog to answer yet.
//   - The badge's own radio pages are no dialogs (Region & radio with its TX power slider, Custom radio, Switch radio):
//     there Enter, Left and Right need the bench console scope too (Lucas: ALLOW_ONAIR_SCOPED). The whole page counts,
//     not one row: a key reaches the page a few ms after the check, so a row could change under it.
//   - A chat or DM screen sends its compose line on Enter and retries a failed message on Tab, with no dialog: there
//     both go only when the open chat is the channel "#badgetest" (Lucas 2026-11-13); every other channel and every DM
//     is refused ("fw mesh send/dm" is the console's way). Typing text is fine.
//   - The other screens where Enter goes on air with no dialog are refused outright: Trace and Discover path, Status
//     and Telemetry (their Again rows), the repeater login and command pages, and the contact page's rows that open
//     them (remote_contact_send_row).
//   - Esc and every other key always go: they only move or go back.
//   - Enter, Left and Right are never held (hold:): LVGL clicks on the release, so a dialog that opened during the hold
//     would take a key checked before it. The stepper checks them again right before the release as a second layer.

// After this long a careful dialog takes Enter (DLG_CAREFUL_ARM_MS, with a margin): "fw ui answer yes" waits for it
#define REMOTE_ANSWER_ARM_MS 1100u

typedef enum { RKEY_OTHER, RKEY_ENTER, RKEY_SIDE, RKEY_TAB } rkey_t; // SIDE: Left or Right

// The one channel a remote Enter may send in, as MeshCore stores its name
#define REMOTE_TEST_CHANNEL "#badgetest"
typedef enum { RDLG_NONE, RDLG_OPEN, RDLG_SETTLING, RDLG_UNREAD } rdlg_t;

typedef enum {
    RANS_OK,
    RANS_HAND_ONLY,    // the dialog's verdict is REFUSE
    RANS_NEEDS_SCOPE,  // a dialog or a radio page that transmits, without the bench console scope
    RANS_UNSURE,       // a dialog is opening, or its state could not be read: ask again shortly
    RANS_NO_DIALOG,    // answer: no dialog is open
    RANS_OTHER_DIALOG, // answer: the open dialog has another confirm label, or is another open (gen)
    RANS_CHAT_TEST,    // a chat send outside #badgetest
    RANS_SENDS,        // an Enter that goes on air with no dialog, outside a chat
} rans_t;

typedef struct {
    rdlg_t               dialog;
    dlg_remote_verdict_t verdict;    // of the open dialog
    char const          *label;      // the open dialog's confirm label
    uint32_t             gen;        // its open number (shell_dialog_info_t.gen)
    int                  top_screen; // nav_current(); only the stepper's check reads it
    bool                 scope_set;  // bench_scope_set()
    char const          *focus;      // what "fw ui" reports as the focus (NULL or "" for none)
    bool                 chat_known; // a chat screen on top whose target was read (bench_chat_on_top)
    bool                 chat_dm;
    char const          *chat_name;  // its channel name, "#" included
} remote_view_t;

// The TCA8418 code for an LVGL key (or a character), 0 when no key types it: lichen_sys_keys_code_for on the badge
typedef uint8_t (*remote_code_for_fn)(uint32_t key, uint8_t *shift_code);
// The kind of the key with TCA8418 code: by the code, so whatever types Enter (also a "\n" in text:) is Enter, and a
// "\t" is Tab. 0 is no key.
rkey_t   remote_key_kind(uint8_t code, remote_code_for_fn code_for);
// A parsed ui_keys sequence: false when a hold: follows an Enter, Left or Right (*at: its index), else true.
// code_of gives a sequence key's TCA8418 code as the stepper sends it (0 for none).
bool     remote_holds_ok(uint32_t const *keys, size_t n, uint8_t (*code_of)(uint32_t key), remote_code_for_fn code_for,
                         size_t *at);
// May a remote key of this kind go down now?
rans_t   remote_key_check(rkey_t key, remote_view_t const *v);
// Is focus (the row label "fw ui" reports) a contact page row whose Enter goes on air with no dialog?
bool     remote_contact_send_row(char const *focus);
// Is screen_id one of the badge's own radio pages (no dialog: Enter, Left and Right there need the bench scope)?
bool     remote_radio_page(int screen_id);
// "yes <label> [#<gen>]" (the text after "yes"): the label (spaces inside kept, the ends trimmed) and the open number when
// given. False when the label is empty or does not fit in cap, or a last word starting with '#' is no number.
bool     remote_answer_parse_yes(char const *arg, char *label, size_t cap, bool *has_gen, uint32_t *gen);
// "fw ui answer yes <want_label> [#<gen>]": may the open dialog be confirmed? An empty or NULL want_label never
// matches; want_gen (NULL: any) must be the dialog's open number. The labels are coarse ("send", "delete" cover
// several dialogs), so the bench always gives the number "fw ui answer" prints.
rans_t   remote_answer_check(remote_view_t const *v, char const *want_label, uint32_t const *want_gen);
// The same right before its Enter goes down (or comes up), which must also find the open it started from (want_gen)
rans_t   remote_answer_still(remote_view_t const *v, char const *want_label, uint32_t want_gen);
// How long "fw ui answer yes" waits before its Enter: up to REMOTE_ANSWER_ARM_MS since the dialog opened
uint32_t remote_answer_wait_ms(uint32_t ms_open);
