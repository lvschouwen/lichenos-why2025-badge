// Which dialogs a remote key may answer (#376, decision on #182): the table is every shell_dialog_countdown and
// menu_set_danger call site in firmware/, by its confirm label and title. '*' in a title stands for what the call site
// prints there (a name, a count). Keep it in step when a dialog's words change: a changed title falls out of its row
// and is refused, which is the safe way to be wrong.
#include "dialog_remote_policy.h"

#include <stddef.h>

#include <string.h>

#if __has_include("sdkconfig.h") // host tests have none and pass CONFIG_LICHEN_BENCH_OPEN by hand
#include "sdkconfig.h"
#endif
// ESP-IDF does not force-include sdkconfig.h: a file that tests the switch must include it itself, or the dev image
// silently builds the release behaviour (#488)
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET)
#error "sdkconfig.h not seen: CONFIG_LICHEN_BENCH_OPEN would read as off"
#endif

// Short names for the table only
#define V_REFUSE DLG_REMOTE_REFUSE
#define V_ALLOW  DLG_REMOTE_ALLOW
#define V_ONAIR  DLG_REMOTE_ALLOW_ONAIR_SCOPED
// The DANGER dialogs that act on this badge only: hand-only, except in a developer image with the bench-open switch
// (#488), where the bench confirms them with `fw ui answer`. The rows that change what a third node does or end the
// console's own way in stay V_REFUSE below, switch or not.
// These labels (continue, remove, forget, ...) must never be reused for a dialog that changes what the badge transmits
// or what a third node does: keep this label set and the ALLOW/ONAIR/REFUSE sets below disjoint.
#ifdef CONFIG_LICHEN_BENCH_OPEN
#define V_LOCAL_DANGER DLG_REMOTE_ALLOW
// Proves in the developer image that this unit saw the switch: tools/check_release_image.sh wants it there and not in a
// release
__attribute__((used, retain)) char const lichen_bench_open_policy_marker[] = "LICHEN-BENCH-OPEN-MARKER";
#else
#define V_LOCAL_DANGER DLG_REMOTE_REFUSE
#endif

// Every row that matches counts and the strictest wins, so a DANGER row with a wide title can never be undercut by an
// ALLOW row that happens to match too (a contact named to look like another dialog's words)
static dlg_remote_row_t const table[] = {
    // DANGER, hand-only (V_LOCAL_DANGER: or the bench's, #488)
    {"reset settings", "*", V_LOCAL_DANGER},   // factory_screen.c: Reset settings
    // "continue" is menu.c's default confirm label too: only the two known titles are the bench's; any other title is unmatched, so refused (a "*" REFUSE row would win strictest-wins over them)
    {"continue", "Erase everything from flash? Settings and every file on the badge.", V_LOCAL_DANGER}, // factory_screen.c
    {"continue", "Erases everything on the card. Format as LICHEN?", V_LOCAL_DANGER},                   // sd_screen.c
    {"erase everything", "*", V_LOCAL_DANGER}, // factory_screen.c: Reset everything, second question
    {"format now", "*", V_LOCAL_DANGER},       // sd_screen.c: format, second question
    {"power off", "*", V_LOCAL_DANGER},        // app_power.c
    {"forget", "*", V_LOCAL_DANGER},           // wifi_screen.c network, meshcore_contact.c repeater password
    {"clear data", "*", V_LOCAL_DANGER},       // app_page.c: Settings > Apps > Clear data (#233): erases an app's settings and files
    {"remove", "*", V_LOCAL_DANGER},           // meshcore_contact.c contact, meshcore_settings.c channel
    {"set radio", "*", V_REFUSE},        // meshcore_rset.c: a third node's radio
    {"restart", "Restart *? It is off the air for a while; log in again after.", V_REFUSE}, // meshcore_rset.c repeater
    {"send", "Send \"*\" to *?", V_REFUSE}, // meshcore_remote.c: an admin command to a third node
    // app_settings.c: confirmed remotely, it cuts off the console that sent the key
    {"switch off", "Switch off updates over Wi-Fi? The Wi-Fi console goes too.", V_REFUSE},
    // app_about.c: a remote key must not end the developer mode that lets it in
    {"switch off", "Switch developer mode off? The developer rows and commands go away.", V_REFUSE},
    // They transmit: only under the bench console scope
    {"send", "Send one request to nearby nodes? It goes on air once, to nodes in direct range.", V_ONAIR}, // discover
    {"send", "Ask * for its telemetry?", V_ONAIR},                          // meshcore_telem.c
    {"send", "Send a flood advert now? The whole mesh hears it.", V_ONAIR}, // meshcore_grid.c
    {"send", "Send an advert now? Nodes in direct range hear it.", V_ONAIR},
    {"switch on", "Ask * for its status every *?", V_ONAIR}, // meshcore_rhist.c
    {"change", "Ask * for its status more often, every *?", V_ONAIR},
    // meshcore_settings.c: the badge's region and a channel's; the tail is a wildcard, as long names cut these titles
    {"set region", "Adverts and messages will only travel in region *", V_ONAIR},
    {"set region", "Messages you send on * are sent to region *", V_ONAIR},
    // The rest
    {"restart", "Restart the badge?", V_ALLOW},                                 // app_power.c
    {"restart", "The radio chip (Wi-Fi and LoRa) is not responding.", V_ALLOW}, // main.c: the C6 is gone
    {"restart", "Switch PSRAM to 120 MHz? Restarts the badge. If 120 MHz fails, it comes back at 80.", V_ALLOW},
    {"restart", "Switch PSRAM to 80 MHz? Restarts the badge.", V_ALLOW}, // app_settings.c
    {"reset", "Reset the route to this contact? The next message floods to find a new one.", V_ALLOW},
    {"reset", "Reset the home order? The tiles go back to their first places.", V_ALLOW}, // app_settings.c
    {"reset", "Reset the tile order? The tiles go back to their first places.", V_ALLOW}, // meshcore_settings.c
    {"delete", "Delete this note?", V_ALLOW},                                             // app_notes.c
    {"delete", "Delete this crash entry? The core dump stays; Clear all erases it.", V_ALLOW},
    {"delete", "Delete *?", V_ALLOW},                   // app_files.c
    {"clear all", "Clear all notifications?", V_ALLOW}, // notify_centre.c (two places)
    {"clear all", "Clear all * notifications?", V_ALLOW},
    {"clear all", "Clear all * crash entries, the crash counter and the core dump?", V_ALLOW}, // app_crashlog.c
    {"clear", "Clear this unread error?", V_ALLOW},                                            // notify_centre.c
    {"open", "Lichen restarted after a crash. Open the crash log?", V_ALLOW},                  // app_crashlog.c
    {"load", "* Load them?", V_ALLOW}, // backup_screen.c: Settings > Backup > Load (Lucas 2026-11-13)
    // app_qr.c
    {"show",
     "The Wi-Fi password is in the code and the code is on the screen for anyone who can see it. Show it?",
     V_ALLOW},
    // Notices: the confirm only closes them
    {"close", "Could not save the reset. Nothing was erased. Try again.", V_ALLOW}, // factory_screen.c
    {"close", "Settings saved to *", V_ALLOW},                                      // backup_screen.c from here on
    {"close", "No SD card in the badge.", V_ALLOW},
    {"close", "No backup found on the SD card (*", V_ALLOW},
    {"close", "Not enough memory. Restart the badge and try again.", V_ALLOW},
    {"close", "The SD card or the settings store could not be used.", V_ALLOW},
    {"close", "The backup matches the settings on the badge. Nothing to load.", V_ALLOW},
    {"close", "Not loaded: * Nothing was changed.", V_ALLOW},
    {"close", "Loaded * Restart the badge to apply all of them.", V_ALLOW},
    {"close", "Only * settings could be loaded. Try again.", V_ALLOW},
    {"close", "Not started: out of memory.", V_ALLOW},
};

bool dlg_remote_glob(char const *pattern, char const *text) {
    char const *star = NULL; // the last '*' seen, and where in text its run ends for now
    char const *mark = NULL;
    while (*text) {
        if (*pattern == '*') {
            star = pattern++;
            mark = text;
        } else if (*pattern == *text) {
            pattern++;
            text++;
        } else if (star) {
            pattern = star + 1; // the last '*' takes one more character
            text    = ++mark;
        } else {
            return false;
        }
    }
    while (*pattern == '*') {
        pattern++;
    }
    return *pattern == '\0';
}

// REFUSE is strictest, then ONAIR, then ALLOW
static int strictness(dlg_remote_verdict_t v) {
    return v == V_ALLOW ? 0 : v == V_ONAIR ? 1 : 2;
}

dlg_remote_verdict_t
    dlg_remote_match(dlg_remote_row_t const *rows, size_t n, char const *confirm_label, char const *title) {
    if (!confirm_label || !title) {
        return V_REFUSE;
    }
    bool                 matched = false;
    dlg_remote_verdict_t verdict = V_ALLOW;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(rows[i].label, confirm_label) != 0 || !dlg_remote_glob(rows[i].title, title)) {
            continue;
        }
        if (!matched || strictness(rows[i].verdict) > strictness(verdict)) {
            verdict = rows[i].verdict;
        }
        matched = true;
    }
    return matched ? verdict : V_REFUSE;
}

dlg_remote_verdict_t dlg_remote_verdict(char const *confirm_label, char const *title) {
    return dlg_remote_match(table, sizeof(table) / sizeof(table[0]), confirm_label, title);
}

char const *dlg_remote_verdict_name(dlg_remote_verdict_t v) {
    switch (v) {
        case V_ALLOW: return "allow";
        case V_ONAIR: return "allow-onair-scoped";
        default: return "refuse";
    }
}

size_t dlg_title_with_tail(char *out, size_t cap, char const *lead, char const *tail) {
    static char const more[] = "...";
    if (cap == 0) {
        return 0;
    }
    lead            = lead ? lead : "";
    tail            = tail ? tail : "";
    size_t lead_len = strlen(lead);
    size_t tail_len = strlen(tail);
    if (tail_len + sizeof(more) > cap) { // not even "..." and the tail fit: the tail alone, cut
        tail_len = tail_len < cap - 1 ? tail_len : cap - 1;
        memcpy(out, tail, tail_len);
        out[tail_len] = '\0';
        return tail_len;
    }
    size_t keep = lead_len;
    bool   cut  = lead_len + tail_len > cap - 1;
    if (cut) {
        keep = cap - 1 - tail_len - (sizeof(more) - 1);
        while (keep > 0 && ((unsigned char)lead[keep] & 0xC0u) == 0x80u) {
            keep--; // lead[keep] is the first byte left out: never inside a character
        }
    }
    size_t pos = 0;
    memcpy(out, lead, keep);
    pos += keep;
    if (cut) {
        memcpy(out + pos, more, sizeof(more) - 1);
        pos += sizeof(more) - 1;
    }
    memcpy(out + pos, tail, tail_len);
    pos      += tail_len;
    out[pos]  = '\0';
    return pos;
}
