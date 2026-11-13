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

#include <assert.h>
#include <stdio.h>
#include <string.h>

static_assert(LV_KEY_NEXT == 9 && LV_KEY_ENTER == 10 && LV_KEY_RIGHT == 19 && LV_KEY_LEFT == 20,
              "remote_answer.c's key codes must match LVGL's");

#define WHO       "ui answer"
#define SCOPE_HOW "fw mesh scope console <region> YES"
#define CHAT_NAME_MAX 40 // a channel name with its '#', or "@" and a contact name (MESH_CHAT_TITLE_MAX is the app's)

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
            if (v->dialog == RDLG_OPEN) {
                snprintf(why, cap, "the open dialog is '%s' #%u", v->label, (unsigned)v->gen);
            } else {
                snprintf(why, cap, "the open dialog is (none)");
            }
            break;
        case RANS_CHAT_TEST: snprintf(why, cap, "remote send only in " REMOTE_TEST_CHANNEL "; use fw mesh send/dm"); break;
        case RANS_SENDS:
            snprintf(why, cap,
                     "this key (screen %d) makes the badge go on air with no dialog: hand-only remotely (remote send "
                     "only in " REMOTE_TEST_CHANNEL ")",
                     v->top_screen);
            break;
        default: snprintf(why, cap, "refused"); break;
    }
}

bool ui_answer_policy_key(uint8_t code) {
    return remote_key_kind(code, lichen_sys_keys_code_for) != RKEY_OTHER;
}

// LVGL task, right before the key goes down: the last look before the keyboard path takes it. A dialog replacing this
// one in the ~40 ms until LVGL reads the key is not caught; a careful one ignores it, being too new. (The look before
// a tap's release only notices a change in that instant: detection, not a gate.)
bool ui_answer_key_ok(uint8_t code, char const *focus, char const *answer_label, uint32_t answer_gen, char *why,
                      size_t cap) {
    rkey_t key = remote_key_kind(code, lichen_sys_keys_code_for);
    if (key == RKEY_OTHER) {
        return true;
    }
    static shell_dialog_info_t info; // LVGL task only: static keeps ~250 B off its stack
    static char                chat[CHAT_NAME_MAX];
    remote_view_t              v;
    read_dialog(&v, &info);
    v.top_screen   = nav_current();
    v.focus        = focus;
    v.chat_known   = bench_chat_on_top(chat, sizeof(chat), &v.chat_dm);
    v.chat_name    = chat;
    v.enter_starts = bench_enter_starts();
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
    console_reply(WHO ": %s%s, confirm '%s' #%u%s, open %u ms, %u s left: %s", dlg_remote_verdict_name(v.verdict),
                  scoped ? (v.scope_set ? " (bench scope set)" : " (bench scope not set: " SCOPE_HOW ")") : "",
                  info.confirm_label, (unsigned)info.gen, info.careful ? ", careful" : "", (unsigned)info.ms_open,
                  (unsigned)info.left_s, info.title);
}

// "yes <label> [#<gen>]": the label is the rest of the line (labels have spaces: "clear all"); #<gen> pins the open
static void answer_yes(char const *arg) {
    char     want[SHELL_DIALOG_LABEL_MAX];
    bool     has_gen;
    uint32_t want_gen;
    if (!remote_answer_parse_yes(arg, want, sizeof(want), &has_gen, &want_gen)) {
        console_reply(WHO ": yes <confirm_label> [#<n>] (both as \"fw " WHO "\" shows them)");
        return;
    }
    shell_dialog_info_t info;
    remote_view_t       v;
    read_dialog(&v, &info);
    rans_t r = remote_answer_check(&v, want, has_gen ? &want_gen : NULL);
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
    char   word[8];
    size_t len;
    while (*args == ' ') {
        args++;
    }
    len = strlen(args);
    while (len && args[len - 1] == ' ') {
        len--; // "no " is no
    }
    snprintf(word, sizeof(word), "%.*s", (int)(len < sizeof(word) - 1 ? len : sizeof(word) - 1), args);
    if (!len) {
        answer_show();
    } else if (len == 2 && strcmp(word, "no") == 0) {
        if (ui_probe_run(WHO, "esc", NULL, 0) >= 0) { // Esc is always allowed
            console_reply(WHO ": Esc sent");
        }
    } else if (strncmp(args, "yes ", 4) == 0 || (len == 3 && strcmp(word, "yes") == 0)) {
        answer_yes(args + 3);
    } else {
        console_reply(WHO " [yes <confirm_label> [#<n>] | no]");
    }
}
