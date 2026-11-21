// Settings > Wi-Fi > an Enterprise (802.1X) network (#496): method, inner method, identity, username, password, CA file
// and server name, then Join. The form lives in one static block in PSRAM and is wiped when the screen goes (Esc or a
// join), so a password never outlives it; the password is write-only (its row says Set or Not set, its entry never starts
// with the old text) and nothing here logs a value. The checks and the rows' texts are in wifi_eap_form.c (host-tested).
#include "esp_attr.h"
#include "lichen_sys_net.h"
#include "lichen_ui.h"
#include "menu.h"
#include "screen_ids.h"
#include "secure_wipe.h"
#include "shell_parts.h"
#include "text_entry.h"
#include "theme.h"
#include "wifi_eap_form.h"

#include <stdio.h>
#include <string.h>

EXT_RAM_BSS_ATTR static lichen_sys_wifi_eap_t form;
static char       ssid_raw[LICHEN_SYS_WIFI_SSID_MAX + 1];
static lv_obj_t  *screen;
static lv_obj_t  *menu;
static lv_obj_t  *rows[EAP_FIELD_COUNT];
static lv_obj_t  *warning;
static void     (*started)(void);

static void refresh(void) {
    char v[EAP_VALUE_MAX + 1];
    for (int f = 0; f < EAP_FIELD_COUNT; f++) {
        eap_form_value(&form, (eap_field_t)f, v, sizeof(v));
        menu_set_value(rows[f], v);
    }
    lv_label_set_text(warning, eap_form_warning(&form));
}

// A status that ends at once: R5 wants one final state, and a stale refusal must not stay after the field was fixed
static void say(char const *verb, char const *ok_text, char const *why, char const *next) {
    menu_status_begin(menu, verb);
    if (why) {
        menu_status_fail(menu, why, next);
    } else {
        menu_status_done(menu, ok_text);
    }
}

static void on_method(void *arg) {
    (void)arg;
    eap_form_cycle_method(&form);
    refresh();
    say("Changing", "", NULL, NULL);
}

static void on_inner(void *arg) {
    (void)arg;
    if (!eap_form_cycle_inner(&form)) {
        say("Checking", "PEAP always uses MSCHAPv2. Choose TTLS for PAP.", NULL, NULL);
        return;
    }
    refresh();
    say("Changing", "", NULL, NULL);
}

// The entry screen is gone by now (text_entry.h); its text is a copy that is wiped after this returns
static void on_text(char const *text, void *arg) {
    eap_field_t f = (eap_field_t)(uintptr_t)arg;
    if (!screen) {
        return;
    }
    if (f == EAP_FIELD_CA && strcmp(text, "/storage/") == 0) {
        text = ""; // the entry started with the directory and was left unchanged: no CA
    }
    char const *err = eap_form_field_error(f, text);
    if (err) {
        say("Saving", NULL, err, "Open the row and try again");
        return;
    }
    eap_form_set(&form, f, text);
    refresh();
    say("Saving", f == EAP_FIELD_PASSWORD ? "Password set" : "", NULL, NULL);
}

static void on_edit(void *arg) {
    eap_field_t f = (eap_field_t)(uintptr_t)arg;
    char        title[96];
    snprintf(title, sizeof(title), "%s %s", eap_form_field_name(f), eap_form_field_hint(f));
    char const *initial = "";
    switch (f) { // never the password: it is write-only, a new one is typed
        case EAP_FIELD_IDENTITY: initial = form.identity; break;
        case EAP_FIELD_USER: initial = form.username; break;
        case EAP_FIELD_CA: initial = form.ca_path[0] ? form.ca_path : "/storage/"; break;
        case EAP_FIELD_SERVER: initial = form.server; break;
        default: break;
    }
    // Hidden (dots, Tab shows) and out of the key log (fw keys on) for every field but the CA path: the login's names are
    // private too, and the posture is tight against remote access
    text_entry_open(title, initial, f != EAP_FIELD_CA, eap_form_field_max(f), on_text, (void *)(uintptr_t)f);
}

static void run_started(void *cb) {
    if (cb) {
        ((void (*)(void))cb)();
    }
}

static void on_join(void *arg) {
    (void)arg;
    char why[120];
    if (!eap_form_check(&form, why, sizeof(why))) {
        say("Checking", NULL, why, "Fix it and join again");
        return;
    }
    if (!lichen_sys_wifi_join_eap(ssid_raw, &form)) {
        say("Joining", NULL, "Could not start the join", "Check the Wi-Fi radio and try again");
        return;
    }
    secure_wipe(&form, sizeof(form)); // the service holds its own copy now
    void (*cb)(void) = started;
    started          = NULL;
    // One async step: this screen pops, then the Wi-Fi screen below starts watching the join
    if (!shell_back_then(run_started, (void *)cb)) {
        shell_back();
        if (cb) {
            cb();
        }
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    secure_wipe(&form, sizeof(form));
    screen = menu = warning = NULL;
    started                 = NULL;
    memset(rows, 0, sizeof(rows));
}

static lv_obj_t *add_row(char const *label, menu_kind_t kind, void (*fn)(void *), void *arg) {
    lv_obj_t *row = menu_add(menu, label, "", fn, arg);
    menu_set_kind(row, kind);
    return row;
}

static lv_obj_t *build(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    shell_style_screen(scr);
    lv_obj_set_style_pad_top(scr, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(scr, THEME_PAD, 0);
    screen = scr;
    char shown[140];
    lichen_sys_wifi_ssid_sanitize(ssid_raw, shown, sizeof(shown));
    menu = menu_create_named(scr, "Enterprise network", shown); // R1: the network's name on its own line
    rows[EAP_FIELD_METHOD] = add_row("Method", MENU_CYCLE, on_method, NULL);
    rows[EAP_FIELD_INNER]  = add_row("Inner method", MENU_CYCLE, on_inner, NULL);
    for (int f = EAP_FIELD_IDENTITY; f < EAP_FIELD_COUNT; f++) {
        rows[f] = add_row(eap_form_field_name((eap_field_t)f), MENU_OPEN, on_edit, (void *)(uintptr_t)f);
    }
    menu_add(menu, "Join", "", on_join, NULL);
    warning = menu_note(menu, "");
    refresh();
    lichen_ui_event_add(scr, on_delete, LV_EVENT_DELETE, NULL);
    return scr;
}

void wifi_eap_screen_open(char const *ssid, void (*done)(void)) {
    if (!shell_screen_open(SCREEN_WIFI_EAP, 0)) { // one already up keeps its form
        snprintf(ssid_raw, sizeof(ssid_raw), "%s", ssid);
        eap_form_init(&form);
        started = done;
    }
    shell_open(build, SCREEN_WIFI_EAP);
}
