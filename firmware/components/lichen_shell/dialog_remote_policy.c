// Which dialogs a remote key may answer (#376, decision on #182): the table is every shell_dialog_countdown and
// menu_set_danger call site in firmware/, by its confirm label and title. '*' in a title stands for what the call site
// prints there (a name, a count). Keep it in step when a dialog's words change: a changed title falls out of its row
// and is refused, which is the safe way to be wrong.
#include "dialog_remote_policy.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    char const          *label;
    char const          *title; // a dlg_remote_glob pattern
    dlg_remote_verdict_t verdict;
} dlg_remote_row_t;

#define REFUSE DLG_REMOTE_REFUSE
#define ALLOW  DLG_REMOTE_ALLOW
#define ONAIR  DLG_REMOTE_ALLOW_ONAIR_SCOPED

// Every row that matches counts and the strictest wins, so a DANGER row with a wide title can never be undercut by an
// ALLOW row that happens to match too (a contact named to look like another dialog's words)
static dlg_remote_row_t const rows[] = {
    // DANGER, hand-only
    {"reset settings", "*", REFUSE},        // factory_screen.c: Reset settings
    {"continue", "*", REFUSE},              // factory_screen.c Reset everything, sd_screen.c format, menu.c's default
    {"erase everything", "*", REFUSE},      // factory_screen.c: Reset everything, second question
    {"format now", "*", REFUSE},            // sd_screen.c: format, second question
    {"power off", "*", REFUSE},             // app_power.c
    {"forget", "*", REFUSE},                // wifi_screen.c network, meshcore_contact.c repeater password
    {"remove", "*", REFUSE},                // meshcore_contact.c contact, meshcore_settings.c channel
    {"set radio", "*", REFUSE},             // meshcore_rset.c: a third node's radio
    {"restart", "Restart *? It is off the air for a while; log in again after.", REFUSE}, // meshcore_rset.c repeater
    {"send", "Send \"*\" to *?", REFUSE},   // meshcore_remote.c: an admin command to a third node
    {"switch off", "Switch off updates over Wi-Fi? The Wi-Fi console goes too.", REFUSE}, // app_settings.c: cuts the remote
    // They transmit: only under the bench console scope
    {"send", "Send one request to nearby nodes? It goes on air once, to nodes in direct range.", ONAIR}, // discover
    {"send", "Ask * for its telemetry?", ONAIR},                         // meshcore_telem.c
    {"send", "Send a flood advert now? The whole mesh hears it.", ONAIR}, // meshcore_grid.c
    {"send", "Send an advert now? Nodes in direct range hear it.", ONAIR},
    {"switch on", "Ask * for its status every *?", ONAIR},              // meshcore_rhist.c
    {"change", "Ask * for its status more often, every *?", ONAIR},
    {"set region", "Adverts and messages will only travel in region *", ONAIR}, // meshcore_settings.c: the badge's region
    {"set region", "Messages you send on * are sent to region *", ONAIR},       // a channel's region
    // The rest
    {"restart", "Restart the badge?", ALLOW},                                   // app_power.c
    {"restart", "The radio chip (Wi-Fi and LoRa) is not responding.", ALLOW},  // main.c: the C6 is gone
    {"restart", "Switch PSRAM to 120 MHz? Restarts the badge. If 120 MHz fails, it comes back at 80.", ALLOW},
    {"restart", "Switch PSRAM to 80 MHz? Restarts the badge.", ALLOW},          // app_settings.c
    {"reset", "Reset the route to this contact? The next message floods to find a new one.", ALLOW},
    {"reset", "Reset the home order? The tiles go back to their first places.", ALLOW}, // app_settings.c
    {"reset", "Reset the tile order? The tiles go back to their first places.", ALLOW}, // meshcore_settings.c
    {"delete", "Delete this note?", ALLOW},                                     // app_notes.c
    {"delete", "Delete this crash entry? The core dump stays; Clear all erases it.", ALLOW},
    {"delete", "Delete *?", ALLOW},                                             // app_files.c
    {"clear all", "Clear all notifications?", ALLOW},                           // notify_centre.c (two places)
    {"clear all", "Clear all * notifications?", ALLOW},
    {"clear all", "Clear all * crash entries, the crash counter and the core dump?", ALLOW}, // app_crashlog.c
    {"clear", "Clear this unread error?", ALLOW},                               // notify_centre.c
    {"open", "Lichen restarted after a crash. Open the crash log?", ALLOW},     // app_crashlog.c
    {"load", "* Load them?", ALLOW}, // backup_screen.c: Settings > Backup > Load (Lucas 2026-11-13)
    {"show", "The Wi-Fi password is in the code and the code is on the screen for anyone who can see it. Show it?",
     ALLOW}, // app_qr.c
    // Notices: the confirm only closes them
    {"close", "Could not save the reset. Nothing was erased. Try again.", ALLOW}, // factory_screen.c
    {"close", "Settings saved to *", ALLOW},                                      // backup_screen.c from here on
    {"close", "No SD card in the badge.", ALLOW},
    {"close", "No backup found on the SD card (*", ALLOW},
    {"close", "Not enough memory. Restart the badge and try again.", ALLOW},
    {"close", "The SD card or the settings store could not be used.", ALLOW},
    {"close", "The backup matches the settings on the badge. Nothing to load.", ALLOW},
    {"close", "Not loaded: * Nothing was changed.", ALLOW},
    {"close", "Loaded * Restart the badge to apply all of them.", ALLOW},
    {"close", "Only * settings could be loaded. Try again.", ALLOW},
    {"close", "Not started: out of memory.", ALLOW},
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
    return v == ALLOW ? 0 : v == ONAIR ? 1 : 2;
}

dlg_remote_verdict_t dlg_remote_verdict(char const *confirm_label, char const *title) {
    if (!confirm_label || !title) {
        return REFUSE;
    }
    bool                 matched = false;
    dlg_remote_verdict_t verdict = ALLOW;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (strcmp(rows[i].label, confirm_label) != 0 || !dlg_remote_glob(rows[i].title, title)) {
            continue;
        }
        if (!matched || strictness(rows[i].verdict) > strictness(verdict)) {
            verdict = rows[i].verdict;
        }
        matched = true;
    }
    return matched ? verdict : REFUSE;
}

char const *dlg_remote_verdict_name(dlg_remote_verdict_t v) {
    switch (v) {
        case ALLOW: return "allow";
        case ONAIR: return "allow-onair-scoped";
        default: return "refuse";
    }
}
