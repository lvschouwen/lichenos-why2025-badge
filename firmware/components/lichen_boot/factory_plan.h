#pragma once

#include <stdbool.h>
#include <stdint.h>

// Factory reset (#66): what each level erases. Pure, host-tested in host_tests/factory_plan_test.c; factory_reset.c
// does the erasing at the next boot, before anything else reads NVS or the storage drive.
//
// Settings: NVS only. In Lichen's namespace (badge_fw) every key goes except FACTORY_KEEP_KEYS, the badge's own
// hardware and health record; every app namespace ("a.<id>") goes, except the keys apps declare as kept (MeshCore's last advert, the airtime limit);
// any other namespace (a library's) is not ours and stays. Not NVS at all and never touched: otadata (which app slot
// boots), the app slots, phy_init, coredump, assets.
// Everything: erased from flash, not just unlinked (the badge has no flash encryption, and a badge handed on must not
// keep the old MeshCore private key). The kept NVS entries are read into RAM, the whole nvs partition is erased and
// they are written back (minus the identity's old NVS copy). The OS's own files on /storage (the C6 image and
// lora.toml, which tools/stage_storage.sh put there) are copied to PSRAM, the whole storage partition is erased and
// formatted, and they are written back. The SD card is never touched.
typedef enum {
    FACTORY_NONE     = 0,
    FACTORY_SETTINGS = 1,
    FACTORY_ALL      = 2,
} factory_level_t;

// The reset is a pending u8 in badge_fw, done at the next boot, so no task is writing while it runs and a power cut
// halfway just runs it again
#define FACTORY_PENDING_KEY     "fr_pending"
#define FACTORY_PENDING_STORAGE 3 // "everything", NVS already done: only the drive is left
#define FACTORY_PENDING_ERASING 4 // the drive's erase began: the PSRAM copy of the OS files died with that boot

typedef struct {
    bool    nvs;       // erase NVS for `level`
    bool    nvs_full;  // by erasing the whole partition and writing the kept entries back (else key by key)
    bool    storage;   // then erase /storage
    bool    restore;   // copy the OS files out first and write them back (false: they are already gone)
    uint8_t after_nvs; // what FACTORY_PENDING_KEY holds once NVS is done (0: erase it)
    factory_level_t level;
} factory_boot_t;

// Set by factory_reset_request at both levels and kept through the erase: the Wi-Fi driver (on the C6) holds its own copy
// of the credentials, and esp_wifi_restore needs the driver up, which it is not at the early-boot erase. wifi_service
// does the restore once the driver is initialised and then erases the key; a failed restore leaves it for the next boot.
#define FACTORY_WIFI_WIPE_KEY "wifi_drv_wipe"

// The drive's erase is tried this many boots before the reset gives up with an error notice
#define FACTORY_TRIES_KEY     "fr_tries"
#define FACTORY_STORAGE_TRIES 3

typedef struct {
    bool    fallback_wipe; // the erase failed: empty the drive through the filesystem instead (not from flash)
    uint8_t pending;       // FACTORY_PENDING_KEY afterwards (0: erase it)
    uint8_t tries;         // FACTORY_TRIES_KEY afterwards (0: erase it)
    bool    notice;        // gave up: "could not fully erase storage"
} factory_storage_next_t;

// After the drive step: erased (the partition erase worked) and tries (failed attempts before this one)
factory_storage_next_t factory_storage_after(bool erased, uint8_t tries);

typedef struct {
    bool key_fallback; // the partition was not erased: erase key by key (the data stays in flash: say so)
    bool carry_in_ram; // the next step could not be saved: do it in this boot from RAM
} factory_nvs_next_t;

// After the NVS step of "everything": erased (deinit, erase and init all worked), flag_saved (the storage stage was
// written to NVS)
factory_nvs_next_t factory_nvs_after(bool erased, bool flag_saved);

// What this boot does for the saved pending value (unknown values: nothing, and the key is cleared)
factory_boot_t factory_boot_plan(uint8_t pending);
// An app's key the reset keeps (lichen_app_t.keep_keys): ns is the app's namespace ("a.<id>"). Before the reset runs.
// False when it does not fit (names over 15 characters, the list of 8 is full). factory_keep_app_keys_clear is for tests.
bool factory_keep_app_key(char const *ns, char const *key);
void factory_keep_app_keys_clear(void);
// True when this NVS entry goes at this level
bool factory_nvs_erases(char const *ns, char const *key, factory_level_t level);
// True for an entry whose name the reset's preview does not print (#288): an app's stored secret ("~" in a.<id>), whose
// name carries a contact's id; the preview counts them instead
bool factory_nvs_name_hidden(char const *ns, char const *key);
// True when a top-level name on /storage survives "everything"
bool factory_storage_keeps(char const *name);
// Write-back order: the C6 bundle's .md5 files after the images, so a half-written bundle never looks complete
bool factory_restore_late(char const *name);
// "settings" or "all" (the console's words)
bool factory_level_parse(char const *word, factory_level_t *out);
char const *factory_level_name(factory_level_t level);

// The combo held at boot (#66): two keys at the ends of the top row, held together for FACTORY_COMBO_HOLD_MS, starting
// within FACTORY_COMBO_WINDOW_MS of the keyboard coming up. It only offers "settings". Pure, host-tested too.
#define FACTORY_COMBO_KEY_A      0x01 // Esc (TCA8418 code)
#define FACTORY_COMBO_KEY_B      0x08 // Backspace
#define FACTORY_COMBO_ENTER      0x3b // Return: confirms on the combo's own screen
#define FACTORY_COMBO_WINDOW_MS  10000
#define FACTORY_COMBO_HOLD_MS    3000
#define FACTORY_COMBO_CONFIRM_MS 30000 // the question cancels itself after this
#define FACTORY_COMBO_ENTER_MS   1000  // Enter counts only this long after the question is up

typedef enum { FC_IDLE, FC_HOLDING, FC_ARMED, FC_OVER } fc_state_t;
typedef enum { FC_EV_NONE, FC_EV_ARM, FC_EV_CONFIRM, FC_EV_CANCEL } fc_event_t;

typedef struct {
    fc_state_t state;
    bool       a_down, b_down;
    uint32_t   since; // FC_HOLDING: both down since; FC_ARMED: the question up since
} factory_combo_t;

void       factory_combo_init(factory_combo_t *c);
// A key event from the TCA8418 at now_ms (ms since the keyboard came up)
fc_event_t factory_combo_key(factory_combo_t *c, uint8_t code, bool pressed, uint32_t now_ms);
// Any key event: an injected one (key_raw_event_t.injected, #391) neither arms nor confirms, since the question is a
// DANGER confirm and hand-only means the TCA8418 only. It cannot touch a hold either; on the open question only its
// Esc counts (cancel, the safe way). The clock still runs.
fc_event_t factory_combo_event(factory_combo_t *c, uint8_t code, bool pressed, bool injected, uint32_t now_ms);
// The clock alone: the hold completing, the question timing out, the window closing
fc_event_t factory_combo_tick(factory_combo_t *c, uint32_t now_ms);
// Keys go to the combo's screen only, not the UI
bool       factory_combo_swallows(factory_combo_t const *c);
