// QR code app (#153): free text or a Wi-Fi network as a QR code for a phone to scan. A contact and a channel have their
// own "Show as QR" row (MeshCore). The screen itself is lichen_api's qr_screen.c.
// A Wi-Fi code carries the password, and the code and the screen can be read by anyone nearby: it only shows behind a
// careful dialog that says so, and the password is never logged or written down (wiped after use).
#include "esp_log.h"
#include "fonts.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "lichen_ui.h"
#include "menu.h"
#include "qr_payload.h"
#include "qr_screen.h"
#include "shell_parts.h"
#include "text_entry.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "qrapp";

#define SSID_MAX      32
#define PASSWORD_MAX  63
#define TEXT_MAX      128
#define ASK_SECONDS   15

// The Wi-Fi flow's values between its three steps; wiped when it ends. LVGL context.
static struct {
    char ssid[SSID_MAX + 1];
    char payload[QR_PAYLOAD_MAX + 1];
} s_wifi;

static void wifi_wipe(void) {
    memset(&s_wifi, 0, sizeof(s_wifi));
}

// ---- text ---------------------------------------------------------------------------------------------------------

static void on_text(char const *text, void *arg) {
    (void)arg;
    if (!text[0]) {
        return;
    }
    if (!qr_screen_open("QR code", "Text", text, NULL)) {
        ESP_LOGW(TAG, "text of %u bytes not shown", (unsigned)strlen(text));
    }
}

static void ask_text(void *arg) {
    (void)arg;
    text_entry_open("Text for the QR code", "", false, TEXT_MAX, on_text, NULL);
}

// ---- Wi-Fi: name, password, then the careful question ---------------------------------------------------------------

static void wipe_later(lv_timer_t *t) {
    (void)t; // repeat count 1: LVGL deletes it after this call
    wifi_wipe();
}

static void shown_answer(bool yes) {
    if (yes) {
        char caption[96];
        snprintf(caption, sizeof(caption), "Scan with a phone camera to join the network. The code holds the password.");
        qr_screen_open("Wi-Fi QR code", s_wifi.ssid, s_wifi.payload, caption);
    }
    wifi_wipe();
}

static void on_password(char const *password, void *arg) {
    (void)arg;
    if (!qr_wifi_payload(s_wifi.ssid, password, s_wifi.payload, sizeof(s_wifi.payload))) {
        ESP_LOGW(TAG, "Wi-Fi code not made");
        wifi_wipe();
        return;
    }
    shell_dialog_t const d = {
        .title         = "The Wi-Fi password is in the code and the code is on the screen for anyone who can see it. Show it?",
        .seconds       = ASK_SECONDS,
        .careful       = true, // the countdown cancels
        .confirm_label = "show",
    };
    if (!shell_dialog_countdown(&d, shown_answer)) {
        wifi_wipe();
        return;
    }
    // A dialog replaced by another one never answers: wipe the password-holding payload anyway, a little after the
    // countdown would have ended (review #153). The QR screen keeps its own copy, wiped when it closes.
    lv_timer_t *t = lv_timer_create(wipe_later, (ASK_SECONDS + 2) * 1000, NULL);
    if (t) {
        lv_timer_set_repeat_count(t, 1);
    } else {
        wifi_wipe();
    }
}

static void on_ssid(char const *ssid, void *arg) {
    (void)arg;
    if (!ssid[0]) {
        return;
    }
    snprintf(s_wifi.ssid, sizeof(s_wifi.ssid), "%s", ssid);
    if (!text_entry_open("Wi-Fi password (empty: open network)", "", true, PASSWORD_MAX, on_password, NULL)) {
        wifi_wipe();
    }
}

static void ask_wifi(void *arg) {
    (void)arg;
    wifi_wipe();
    text_entry_open("Wi-Fi network name", "", false, SSID_MAX, on_ssid, NULL);
}

// ---- the app --------------------------------------------------------------------------------------------------------

static void open_qr(lv_obj_t *screen) {
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *menu = menu_create(screen, "QR code");
    menu_add(menu, "Text...", NULL, ask_text, NULL);
    menu_add(menu, "Wi-Fi network...", NULL, ask_wifi, NULL);
    menu_note(menu, "A contact or a channel has its own Show as QR row, under Share.");
    shell_page_hint(screen, "Enter", "choose", NULL);
}

// Three finder squares, like the corners of a code
static void icon_qr(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    int sq = size_px / 5, gap = size_px / 12;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, sq, sq);
        lv_obj_set_style_border_width(box, size_px >= 64 ? 4 : 2, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(i == 0 ? THEME_AMBER : THEME_TEAL), 0);
        lv_obj_align(box, LV_ALIGN_CENTER, (i == 1 ? 1 : -1) * (sq + gap) / 2, (i == 2 ? 1 : -1) * (sq + gap) / 2);
    }
}

static lichen_app_t const qr_app = {
    .name      = "QR code",
    .id        = "qrcode",
    .draw_icon = icon_qr,
    .open      = open_qr,
};

// fw qr <text>: for the bench, shows the text as a code
static char s_cmd_text[QR_PAYLOAD_MAX + 1];

static void cmd_open(void *arg) {
    (void)arg;
    qr_screen_open("QR code", "Text", s_cmd_text, NULL);
}

static void cmd_qr(char const *args) {
    if (!qr_payload_ok(args)) {
        console_reply("qr: usage: qr <text> (1..%d bytes)", QR_PAYLOAD_MAX);
        return;
    }
    snprintf(s_cmd_text, sizeof(s_cmd_text), "%s", args);
    if (lichen_ui_call(cmd_open, NULL)) {
        console_reply("qr: showing %u bytes", (unsigned)strlen(args));
    } else {
        console_reply("qr: the screen is busy, try again");
    }
}

void qr_app_register(void) {
    lichen_register_app(&qr_app);
    console_register("qr", "qr <text> (show a QR code, #153)", cmd_qr);
}
