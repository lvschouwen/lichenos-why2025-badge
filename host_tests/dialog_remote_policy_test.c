// Host test: which dialogs a remote key may answer (#376, #182). Every row is checked with the title its call site
// really prints, so a row whose pattern drifted from its dialog fails here.
#include "dialog_remote_policy.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define R DLG_REMOTE_REFUSE
#define A DLG_REMOTE_ALLOW
#define O DLG_REMOTE_ALLOW_ONAIR_SCOPED

typedef struct {
    char const          *label;
    char const          *title;
    dlg_remote_verdict_t want;
} case_t;

// One or more per table row, as the call sites fill them in
static case_t const cases[] = {
    {"reset settings", "Reset all settings and restart? MeshCore and files stay.", R},
    {"continue", "Erase everything from flash? Settings and every file on the badge.", R},
    {"continue", "Erases everything on the card. Format as LICHEN?", R},
    {"continue", "Are you sure?", R},
    {"erase everything",
     "Sure? The MeshCore identity is erased from flash for good: nobody's contacts will know this badge.", R},
    {"format now", "Sure? All files on the card are lost.", R},
    {"power off", "Power off the badge?", R},
    {"forget", "Forget Home? Its password is deleted.", R},
    {"forget", "Forget the saved password of this repeater or room? Log in asks for it again.", R},
    {"remove", "Remove Alice from MeshCore? The chat stays.", R},
    {"remove", "Remove #badgetest? Messages already received stay.", R},
    {"set radio", "Set the radio of Rep1 to 869.618 MHz, BW 62.5, SF 8, CR 8?", R},
    {"restart", "Restart Rep1? It is off the air for a while; log in again after.", R},
    {"restart", "Restart the badge? It is off the air for a while; log in again after.", R}, // a repeater so named
    {"send", "Send \"reboot\" to Rep1?", R},
    {"send", "Send \"set radio 869.618,62.5,8,8\" to Rep1?", R},
    {"switch off", "Switch off updates over Wi-Fi? The Wi-Fi console goes too.", R},
    {"send", "Send one request to nearby nodes? It goes on air once, to nodes in direct range.", O},
    {"send", "Ask Alice for its telemetry?", O},
    {"send", "Ask (no name) for its telemetry?", O},
    {"send", "Send a flood advert now? The whole mesh hears it.", O},
    {"send", "Send an advert now? Nodes in direct range hear it.", O},
    {"switch on", "Ask Rep1 for its status every 2 h?", O},
    {"change", "Ask Rep1 for its status more often, every 30 min?", O},
    {"set region",
     "Adverts and messages will only travel in region nl-ams. Contacts outside it can no longer reach you.", O},
    {"set region", "Messages you send on #badgetest are sent to region nl-ams. Repeaters outside it do not pass them "
                   "on, so members who only reach you through them stop receiving them.", O},
    {"restart", "Restart the badge?", A},
    {"restart", "The radio chip (Wi-Fi and LoRa) is not responding.", A},
    {"restart", "Switch PSRAM to 120 MHz? Restarts the badge. If 120 MHz fails, it comes back at 80.", A},
    {"restart", "Switch PSRAM to 80 MHz? Restarts the badge.", A},
    {"reset", "Reset the route to this contact? The next message floods to find a new one.", A},
    {"reset", "Reset the home order? The tiles go back to their first places.", A},
    {"reset", "Reset the tile order? The tiles go back to their first places.", A},
    {"delete", "Delete this note?", A},
    {"delete", "Delete this crash entry? The core dump stays; Clear all erases it.", A},
    {"delete", "Delete notes.txt?", A},
    {"delete", "Delete photos (folder)?", A},
    {"clear all", "Clear all notifications?", A},
    {"clear all", "Clear all 7 notifications?", A},
    {"clear all", "Clear all 3 crash entries, the crash counter and the core dump?", A},
    {"clear", "Clear this unread error?", A},
    {"load", "3 settings differ from the badge. Load them?", A},
    {"open", "Lichen restarted after a crash. Open the crash log?", A},
    {"show", "The Wi-Fi password is in the code and the code is on the screen for anyone who can see it. Show it?", A},
    {"close", "Could not save the reset. Nothing was erased. Try again.", A},
    {"close", "Settings saved to /sd/lichen/settings.json.", A},
    {"close", "No SD card in the badge.", A},
    {"close", "No backup found on the SD card (/sd/lichen/settings.json).", A},
    {"close", "Not enough memory. Restart the badge and try again.", A},
    {"close", "The SD card or the settings store could not be used.", A},
    {"close", "The backup matches the settings on the badge. Nothing to load.", A},
    {"close", "Not loaded: line 3 is not a setting. Nothing was changed.", A},
    {"close", "Loaded 4 settings. Restart the badge to apply all of them.", A},
    {"close", "Only 2 settings could be loaded. Try again.", A},
    {"close", "Not started: out of memory.", A},
};

static void test_rows(void) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dlg_remote_verdict_t got = dlg_remote_verdict(cases[i].label, cases[i].title);
        if (got != cases[i].want) {
            printf("FAIL row %zu: \"%s\" / \"%s\": %s, want %s\n", i, cases[i].label, cases[i].title,
                   dlg_remote_verdict_name(got), dlg_remote_verdict_name(cases[i].want));
            failures++;
        }
    }
}

// Fail closed: what no row names is hand-only
static void test_unknown(void) {
    CHECK(dlg_remote_verdict("launch", "Launch the rocket?") == R);
    CHECK(dlg_remote_verdict("restart", "Restart the badge now?") == R);   // a known label, an unknown title
    CHECK(dlg_remote_verdict("delete", "Erase this note?") == R);
    CHECK(dlg_remote_verdict("load", "Load the backup now?") == R);       // the load row needs its own ending
    CHECK(dlg_remote_verdict("close", "Your files were deleted.") == R);
    CHECK(dlg_remote_verdict("", "Restart the badge?") == R);
    CHECK(dlg_remote_verdict("Restart", "Restart the badge?") == R);       // labels match exactly
    CHECK(dlg_remote_verdict("restart", "") == R);
    CHECK(dlg_remote_verdict(NULL, "Restart the badge?") == R);
    CHECK(dlg_remote_verdict("restart", NULL) == R);
    CHECK(dlg_remote_verdict(NULL, NULL) == R);
    // The dialog layer keeps 191 characters of a title: a cut-off one no longer ends right and is refused
    CHECK(dlg_remote_verdict("delete", "Delete notes.tx") == R);
}

// A name chosen to look like another dialog never lifts a DANGER one
static void test_strictest_wins(void) {
    CHECK(dlg_remote_verdict("send", "Send \"x\" to Ask Alice for its telemetry?") == R);
    CHECK(dlg_remote_verdict("delete", "Delete this crash entry? The core dump stays; Clear all erases it.") == A);
    CHECK(dlg_remote_verdict("send", "Ask \"x\" to y? for its telemetry?") == O); // not a CLI title: no `Send "`
}

static void test_glob(void) {
    CHECK(dlg_remote_glob("", ""));
    CHECK(!dlg_remote_glob("", "a"));
    CHECK(dlg_remote_glob("*", ""));
    CHECK(dlg_remote_glob("*", "anything"));
    CHECK(dlg_remote_glob("a*c", "abbbc"));
    CHECK(dlg_remote_glob("a*c", "ac"));
    CHECK(!dlg_remote_glob("a*c", "acb"));
    CHECK(dlg_remote_glob("a*b*c", "a-b-b-c"));
    CHECK(dlg_remote_glob("a*bc", "abcbc")); // the first "bc" is not the end: backtrack
    CHECK(!dlg_remote_glob("abc", "ab"));
    CHECK(!dlg_remote_glob("ab", "abc"));
}

static void test_names(void) {
    CHECK(strcmp(dlg_remote_verdict_name(R), "refuse") == 0);
    CHECK(strcmp(dlg_remote_verdict_name(A), "allow") == 0);
    CHECK(strcmp(dlg_remote_verdict_name(O), "allow-onair-scoped") == 0);
    CHECK(strcmp(dlg_remote_verdict_name((dlg_remote_verdict_t)99), "refuse") == 0);
}

int main(void) {
    test_rows();
    test_unknown();
    test_strictest_wins();
    test_glob();
    test_names();
    if (failures) {
        printf("dialog_remote_policy_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("dialog_remote_policy_test: ok\n");
    return 0;
}
