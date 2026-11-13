// The remote key rules of #377: see remote_answer.h
#include "remote_answer.h"

#include "screen_ids.h"

#include <stddef.h>
#include <string.h>

bool remote_radio_page(int screen_id) {
    return screen_id == SCREEN_RADIO || screen_id == SCREEN_CUSTOM || screen_id == SCREEN_SWITCH;
}

static rans_t by_verdict(dlg_remote_verdict_t verdict, bool scope_set) {
    if (verdict == DLG_REMOTE_ALLOW) {
        return RANS_OK;
    }
    if (verdict == DLG_REMOTE_ALLOW_ONAIR_SCOPED) {
        return scope_set ? RANS_OK : RANS_NEEDS_SCOPE;
    }
    return RANS_HAND_ONLY; // REFUSE and anything else: fail closed
}

rans_t remote_key_check(rkey_t key, remote_view_t const *v) {
    if (key == RKEY_OTHER) {
        return RANS_OK;
    }
    if (key == RKEY_ENTER && v->dialog != RDLG_NONE) {
        // The Enter goes to the dialog (or to one about to be built), not to the page below it
        return v->dialog == RDLG_OPEN ? by_verdict(v->verdict, v->scope_set) : RANS_UNSURE;
    }
    // Left and Right reach the page even under a dialog's state being unsure: its rule holds either way
    return remote_radio_page(v->top_screen) && !v->scope_set ? RANS_NEEDS_SCOPE : RANS_OK;
}

rans_t remote_answer_check(remote_view_t const *v, char const *want_label) {
    if (v->dialog == RDLG_NONE) {
        return RANS_NO_DIALOG;
    }
    if (v->dialog != RDLG_OPEN) {
        return RANS_UNSURE;
    }
    if (!want_label || !*want_label || !v->label || strcmp(v->label, want_label) != 0) {
        return RANS_OTHER_DIALOG; // a stale or different dialog is never confirmed
    }
    return by_verdict(v->verdict, v->scope_set);
}

rans_t remote_answer_still(remote_view_t const *v, char const *want_label, uint32_t want_gen) {
    rans_t r = remote_answer_check(v, want_label);
    if (r == RANS_OK && v->gen != want_gen) {
        return RANS_OTHER_DIALOG; // the same words, but opened again since: not the dialog that was asked about
    }
    return r == RANS_NO_DIALOG ? RANS_OTHER_DIALOG : r; // it closed meanwhile: also not the one asked about
}

uint32_t remote_answer_wait_ms(uint32_t ms_open) {
    return ms_open >= REMOTE_ANSWER_ARM_MS ? 0 : REMOTE_ANSWER_ARM_MS - ms_open;
}
