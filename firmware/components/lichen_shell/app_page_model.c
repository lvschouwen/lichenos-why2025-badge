// See app_page_model.h (#233)
#include "app_page_model.h"

#include "lichen_caps.h"
#include "ui_text.h"

#include <stdio.h>
#include <string.h>

app_page_rows_t app_page_rows(app_page_app_t const *app) {
    app_page_rows_t r = {0};
    bool            system = (app->caps & CAP_SYSTEM) != 0;
    r.listed  = app->has_hooks || !system;
    r.crashes = true;
    if (system) {
        return r;
    }
    r.data       = true;
    r.clear      = !app->secret_storage;
    r.clear_note = app->secret_storage;
    r.background = true;
    r.bg         = !app->has_background ? APP_BG_NONE : app->exempt ? APP_BG_LOCKED : APP_BG_SWITCH;
    return r;
}

// Indexed by capability bit (lichen_caps.h); the test walks 0 .. CAP_COUNT - 1
static char const *const cap_words[CAP_COUNT] = {
    [0]  = "LoRa sending",
    [1]  = "the LoRa radio",
    [2]  = "borrowing the LoRa radio",
    [3]  = "the SD card",
    [4]  = "the network",
    [5]  = "its own secrets",
    [6]  = "the raw keyboard",
    [7]  = "console commands",
    [8]  = "restarting the badge",
    [9]  = "background tasks",
    [10] = "the vibration motor",
    [11] = "the sensors",
    [12] = "notifications",
    [13] = "radio scans",
    [14] = "Bluetooth",
    [15] = "audio",
    [16] = "the battery",
    [17] = NULL, // CAP_SYSTEM: said as a whole
};
_Static_assert(CAP_COUNT == 18u, "a new capability needs its words in cap_words (app_page_model.c)");

char const *app_page_cap_words(unsigned bit) {
    return bit < CAP_COUNT ? cap_words[bit] : NULL;
}

static size_t put(char *out, size_t cap, char const *text) {
    if (cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap, "%s", text);
    return n < 0 ? 0 : (size_t)n >= cap ? cap - 1 : (size_t)n;
}

size_t app_page_caps_text(uint32_t caps, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    if (caps & CAP_SYSTEM) {
        return put(out, cap, "Part of Lichen: it may use everything.");
    }
    size_t len = (size_t)snprintf(out, cap, "Uses ");
    if (len >= cap) {
        return put(out, cap, "Uses");
    }
    bool any = false;
    for (unsigned bit = 0; bit < CAP_COUNT; bit++) {
        if (!(caps & (1u << bit)) || !cap_words[bit]) {
            continue;
        }
        int n = snprintf(out + len, cap - len, "%s%s", any ? ", " : "", cap_words[bit]);
        if (n < 0 || (size_t)n >= cap - len) { // too long for the buffer: end where it fits, as a sentence
            len = cap - 1;
            break;
        }
        len += (size_t)n;
        any = true;
    }
    if (!any) {
        return put(out, cap, "Uses nothing special.");
    }
    if (len + 2 <= cap) {
        out[len++] = '.';
        out[len]   = '\0';
    }
    return len;
}

bool app_page_bg_running(app_life_t const *life) {
    switch ((life_state_t)life->state) {
    case LIFE_BACKGROUND: return true;
    case LIFE_OPEN:
    case LIFE_SUSPENDED:  return life->rest == LIFE_BACKGROUND;
    default:              return false;
    }
}

size_t app_page_bg_text(app_page_bg_t mode, bool allowed, bool running, bool crashes_off, char *out, size_t cap) {
    if (mode == APP_BG_NONE) {
        return put(out, cap, "None");
    }
    if (mode == APP_BG_LOCKED) {
        return put(out, cap, "Always on");
    }
    if (allowed) {
        return put(out, cap, running ? "On" : "On after restart");
    }
    return put(out, cap, running ? "Off after restart" : crashes_off ? "Off after crashes" : "Off");
}

size_t app_page_crash_text(unsigned crashes, unsigned window, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    int n = crashes ? snprintf(out, cap, "%u in the last %u boots", crashes, window)
                    : snprintf(out, cap, "None in the last %u boots", window);
    return n < 0 ? 0 : (size_t)n >= cap ? cap - 1 : (size_t)n;
}

size_t app_page_data_text(lichen_app_data_t const *d, char *out, size_t cap) {
    uint64_t total = (uint64_t)d->settings_bytes + d->files_bytes;
    return total ? ui_size_text(out, cap, total) : put(out, cap, "None");
}

size_t app_page_sd_text(lichen_app_data_t const *d, char *out, size_t cap) {
    if (!d->sd_known) {
        return put(out, cap, "No card");
    }
    return d->sd_bytes ? ui_size_text(out, cap, d->sd_bytes) : put(out, cap, "None");
}

size_t app_page_clear_question(char const *name, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap, "Clear all data of %s? Its settings and files are erased and cannot come back.",
                     name && name[0] ? name : "this app");
    return n < 0 ? 0 : (size_t)n >= cap ? cap - 1 : (size_t)n;
}

void app_page_clear_msg(lichen_app_clear_t const *c, bool bg_running, app_page_clear_msg_t *out) {
    memset(out, 0, sizeof(*out));
    out->ok = c->settings_ok && c->files_ok && c->sd_ok;
    if (out->ok) {
        char size[24];
        ui_size_text(size, sizeof(size), c->bytes);
        snprintf(out->text, sizeof(out->text), "Cleared%s%s.%s", c->bytes ? " " : "", c->bytes ? size : "",
                 bg_running ? " Restart the badge so the app starts fresh." : "");
        return;
    }
    snprintf(out->text, sizeof(out->text), "%s could not be cleared",
             !c->settings_ok ? "Its settings" : !c->files_ok ? "Its files" : "Its files on the SD card");
    snprintf(out->next, sizeof(out->next), "Try Clear data again.");
}
