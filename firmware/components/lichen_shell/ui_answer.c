// "fw ui answer" (#377, closes #182): a dialog answered over the console by its remote policy, and the stepper's check
// before every remote Enter, Left or Right ("fw keys send" too). The rules are remote_answer.c (host-tested); this
// reads the dialog (shell_dialog_info), the screen on top and the bench console scope (bench_scope.h), and acts.
#include "bench_scope.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_sys_keys.h"
#include "lvgl.h"
#include "nav.h"
#include "remote_answer.h"
#include "shell_parts.h"

#include <stdio.h>
#include <string.h>

#define WHO       "ui answer"
#define SCOPE_HOW "fw mesh scope console <region> YES"

// Any task: the dialog part of the view, with its strings in info
static void read_dialog(remote_view_t *v, shell_dialog_info_t *info) {
    bool read = shell_dialog_info(info);
    *v        = (remote_view_t){
               .dialog     = !read ? RDLG_UNREAD : info->open ? RDLG_OPEN : info->settling ? RDLG_SETTLING : RDLG_NONE,
               .verdict    = info->open ? dlg_remote_verdict(info->confirm_label, info->title) : DLG_REMOTE_REFUSE,
               .label      = info->confirm_label,
               .gen        = info->gen,
               .top_screen = -1,
               .scope_set  = bench_scope_set(),
    };
}

// page: the refusal is the radio page's, not the dialog's
static void why_text(rans_t r, remote_view_t const *v, bool page, char *why, size_t cap) {
    switch (r) {
        case RANS_HAND_ONLY: snprintf(why, cap, "dialog '%s' is hand-only", v->label); break;
        case RANS_NEEDS_SCOPE:
            if (page) {
                snprintf(why, cap, "the badge's radio page (screen %d) only under the bench console scope (" SCOPE_HOW ")",
                         v->top_screen);
            } else {
                snprintf(why, cap, "dialog '%s' transmits: only under the bench console scope (" SCOPE_HOW ")", v->label);
            }
            break;
        case RANS_UNSURE: snprintf(why, cap, "a dialog is opening (or could not be read); try again"); break;
        case RANS_NO_DIALOG: snprintf(why, cap, "no dialog open"); break;
        case RANS_OTHER_DIALOG:
            snprintf(why, cap, "the open dialog is '%s'", v->dialog == RDLG_OPEN ? v->label : "(none)");
            break;
        default: snprintf(why, cap, "refused"); break;
    }
}

static rkey_t kind_of(uint8_t code) {
    uint8_t shift;
    if (!code) {
        return RKEY_OTHER;
    }
    if (code == lichen_sys_keys_code_for(LV_KEY_ENTER, &shift)) {
        return RKEY_ENTER;
    }
    bool side = code == lichen_sys_keys_code_for(LV_KEY_LEFT, &shift) ||
                code == lichen_sys_keys_code_for(LV_KEY_RIGHT, &shift);
    return side ? RKEY_SIDE : RKEY_OTHER;
}

// LVGL task, right before the key goes down: this is the last look before the keyboard path takes it (a dialog
// replacing this one in the few ms until LVGL reads the key is not caught; a careful one ignores it, being too new)
bool ui_answer_key_ok(uint8_t code, char const *answer_label, uint32_t answer_gen, char *why, size_t cap) {
    rkey_t key = kind_of(code);
    if (key == RKEY_OTHER) {
        return true;
    }
    shell_dialog_info_t info;
    remote_view_t       v;
    read_dialog(&v, &info);
    v.top_screen = nav_current();
    rans_t r     = answer_label && key == RKEY_ENTER ? remote_answer_still(&v, answer_label, answer_gen)
                                                     : remote_key_check(key, &v);
    if (r == RANS_OK) {
        return true;
    }
    why_text(r, &v, !(key == RKEY_ENTER && v.dialog != RDLG_NONE), why, cap);
    return false;
}

static void answer_show(void) {
    shell_dialog_info_t info;
    remote_view_t       v;
    read_dialog(&v, &info);
    if (v.dialog != RDLG_OPEN) {
        char why[96];
        why_text(v.dialog == RDLG_NONE ? RANS_NO_DIALOG : RANS_UNSURE, &v, false, why, sizeof(why));
        console_reply(WHO ": %s", why);
        return;
    }
    bool scoped = v.verdict == DLG_REMOTE_ALLOW_ONAIR_SCOPED;
    console_reply(WHO ": %s%s, confirm '%s'%s, open %u ms, %u s left: %s", dlg_remote_verdict_name(v.verdict),
                  scoped ? (v.scope_set ? " (bench scope set)" : " (bench scope not set: " SCOPE_HOW ")") : "",
                  info.confirm_label, info.careful ? ", careful" : "", (unsigned)info.ms_open, (unsigned)info.left_s,
                  info.title);
}

// "yes <label>": the label is the rest of the line (labels have spaces: "clear all"), trailing spaces cut
static void answer_yes(char const *arg) {
    char want[SHELL_DIALOG_LABEL_MAX];
    while (*arg == ' ') {
        arg++;
    }
    size_t len = strlen(arg);
    while (len && arg[len - 1] == ' ') {
        len--;
    }
    if (!len || len >= sizeof(want)) {
        console_reply(WHO ": yes <confirm_label> (as \"fw " WHO "\" shows it)");
        return;
    }
    memcpy(want, arg, len);
    want[len] = '\0';
    shell_dialog_info_t info;
    remote_view_t       v;
    read_dialog(&v, &info);
    rans_t r = remote_answer_check(&v, want);
    if (r != RANS_OK) {
        char why[192];
        why_text(r, &v, false, why, sizeof(why));
        console_reply(WHO ": %s; not confirmed", why);
        return;
    }
    uint32_t wait = remote_answer_wait_ms(info.ms_open);
    if (wait) {
        vTaskDelay(pdMS_TO_TICKS(wait) + 1); // a careful dialog takes Enter only once armed
    }
    uint32_t gen = info.gen;
    if (ui_probe_run(WHO, "enter", want, gen) < 0) {
        return; // the stepper said why (the dialog changed, the scope went ...)
    }
    read_dialog(&v, &info);
    if (v.dialog == RDLG_OPEN && info.gen == gen) {
        console_reply(WHO ": Enter sent, but '%s' is still open", want);
        return;
    }
    console_reply(WHO ": confirmed '%s'", want);
}

void ui_answer_cmd(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (!*args) {
        answer_show();
    } else if (strcmp(args, "no") == 0) {
        if (ui_probe_run(WHO, "esc", NULL, 0) >= 0) { // Esc is always allowed
            console_reply(WHO ": Esc sent");
        }
    } else if (strncmp(args, "yes ", 4) == 0 || strcmp(args, "yes") == 0) {
        answer_yes(args + 3);
    } else {
        console_reply(WHO " [yes <confirm_label> | no]");
    }
}
