#pragma once

#include <stdbool.h>

// May a key sent over Wi-Fi ("fw keys send", #182) answer the dialog that is open? Lucas 2026-11-13: every dialog but
// the DANGER ones, which stay hand-only. A dialog is known by its confirm label and its title, read from the call sites
// (dialog_remote_policy.c lists them); one that matches no row is refused, so a new dialog stays hand-only until it is
// added here. Pure, host-tested in host_tests/dialog_remote_policy_test.c. The developer-mode and bench-scope checks
// are the caller's (#377): this only says which of them a dialog needs.

typedef enum {
    DLG_REMOTE_REFUSE,            // hand-only (DANGER), unknown, or a NULL label or title
    DLG_REMOTE_ALLOW,             // a remote Enter may answer it
    DLG_REMOTE_ALLOW_ONAIR_SCOPED // it makes the badge transmit: only under the bench console scope
} dlg_remote_verdict_t;

// confirm_label and title as shell_dialog_t has them (the dialog layer's truncated copies are fine: a cut-off title
// that no longer matches is refused)
dlg_remote_verdict_t dlg_remote_verdict(char const *confirm_label, char const *title);

// "refuse", "allow" or "allow-onair-scoped"
char const *dlg_remote_verdict_name(dlg_remote_verdict_t v);

// '*' matches any run of characters (also none); every other character itself. Exposed for the host test.
bool dlg_remote_glob(char const *pattern, char const *text);
