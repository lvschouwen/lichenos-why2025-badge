#pragma once
// Settings > Apps (#233), the pure part: which rows an app's page gets and every text on it. ESP-free and LVGL-free,
// host-tested in host_tests/app_page_model_test.c; app_page.c builds the rows from these.

#include "app_life.h"
#include "app_data_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the page needs to know of the app's descriptor
typedef struct {
    uint32_t caps;           // lichen_app_t.caps
    bool     has_hooks;      // it has a settings or an about page of its own
    bool     has_background; // start_background is set
    bool     exempt;         // safe_mode_exempt: the OS keeps its background part on (MeshCore)
    bool     secret_storage; // storage_secret: its directory holds an identity and keys
} app_page_app_t;

typedef enum {
    APP_BG_NONE,   // no background part: the row says so
    APP_BG_SWITCH, // the person may switch it on and off
    APP_BG_LOCKED, // the OS keeps it on (MeshCore, owner decision 2026-11-14: only a person with a way to switch it
                   // back on may switch the main app off; #233 keeps the exemption and shows it)
} app_page_bg_t;

typedef struct {
    bool          listed;     // under Settings > Apps at all
    bool          data;       // the data size rows
    bool          clear;      // Clear data (careful dialog)
    bool          clear_note; // no Clear data, and why: the app's identity and keys go only with a factory reset
    bool          background; // the Background row (any mode, APP_BG_NONE included)
    app_page_bg_t bg;
    bool          crashes;    // the Crashes row
} app_page_rows_t;

// The OS's own apps (CAP_SYSTEM: Settings, About, Files, Power ...) are listed only when they have a page of their own,
// and get their capabilities and crash count but no data rows, no Clear data and no Background switch: the OS does not
// clear itself.
app_page_rows_t app_page_rows(app_page_app_t const *app);

// "Uses LoRa radio, SD card." / "Uses nothing special." / "Part of Lichen: it may use everything." (R4: words, never
// CAP_ names). The text is at most APP_PAGE_CAPS_MAX bytes with its NUL.
#define APP_PAGE_CAPS_MAX 320
size_t app_page_caps_text(uint32_t caps, char *out, size_t cap);
// The plain words of capability bit (0 .. CAP_COUNT - 1); NULL for CAP_SYSTEM and past the end. Pinned by the test so a
// new capability cannot ship without its words.
char const *app_page_cap_words(unsigned bit);

// Is the app's background part running now? From its lifecycle record: BACKGROUND, or a screen open over a rest state
// of BACKGROUND (OPEN, SUSPENDED); not once a stop began or ended.
bool app_page_bg_running(app_life_t const *life);

// The Background row's value. allowed: the app is not on the switched-off list (what the switch shows); running:
// app_page_bg_running; crashes_off: its life record says safe mode switched it off. A change applies at the next boot,
// so an allowed part that is not running reads "On after restart" and a running one that is not allowed "Off after
// restart".
size_t app_page_bg_text(app_page_bg_t mode, bool allowed, bool running, bool crashes_off, char *out, size_t cap);

// "None in the last 3 boots" / "1 in the last 3 boots" / "2 in the last 3 boots"
size_t app_page_crash_text(unsigned crashes, unsigned window, char *out, size_t cap);

// Does a clear leave the app holding old state? Every state but REGISTERED (nothing of it ever ran), STOPPED and FAULTED
// (nothing runs): a background part, an open screen, a suspended one under Settings or a stop in progress keep their
// settings in RAM and may write them back, and the app's own rows on this page show the old values until a restart.
bool app_page_needs_restart(app_life_t const *life);

// The Data row: the app's settings and files in the badge, "None" when empty; and the SD card row ("No card" when none,
// "Unknown" when the card was busy)
size_t app_page_data_text(lichen_app_data_t const *d, char *out, size_t cap);
size_t app_page_sd_text(lichen_app_data_t const *d, char *out, size_t cap);

// The careful dialog's question: names the app and what goes. Short enough that the dialog keeps all of it.
size_t app_page_clear_question(char const *name, char *out, size_t cap);
// The status line's ending (R5): ok gives text "Cleared 12 KB." (with the restart advice when restart is true, see
// app_page_needs_restart); failed gives why (text) and next for ui_status_fail_text.
typedef struct {
    bool ok;
    char text[128]; // done: the whole line; failed: why (the reason, plain words)
    char next[64]; // failed: what to do next; "" when done
} app_page_clear_msg_t;
void app_page_clear_msg(lichen_app_clear_t const *c, bool restart, app_page_clear_msg_t *out);

#ifdef __cplusplus
}
#endif
