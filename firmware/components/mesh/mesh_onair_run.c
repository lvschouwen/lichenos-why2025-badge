// The console's on-air allow list on the badge (#289): see mesh_onair_run.h
#include "mesh_onair_run.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fw_console.h"
#include "lichen_settings.h"
#include "meshcore_run.h"

#include <stdio.h>
#include <string.h>

static char const TAG[]      = "meshcore";
static char const KEY_ON[]   = "onair_on";   // u8, 1 = on; missing = off (a user badge)
static char const KEY_LIST[] = "onair_list"; // onair_encode's text; erased when the list is empty

// The live copy: read by every console send and the mesh task's polls, written only by "fw onair" (console runs, one at
// a time). A mutex, never held over a setting write or a console reply, so it never waits on anything else.
static StaticSemaphore_t           onair_lock_mem;
static SemaphoreHandle_t           onair_lock; // NULL until mesh_onair_init: the state is then off, as on a user badge
EXT_RAM_BSS_ATTR static onair_list_t onair_list;
static bool                        onair_on;
static bool                        onair_damaged; // the saved list did not decode: it is empty (refuses every send)
// The refusal with its target, for the console run that asked (console.c serialises the runs)
static char onair_refused[ONAIR_REFUSED_MAX];

bool mesh_onair_on(void) {
    SemaphoreHandle_t lock = __atomic_load_n(&onair_lock, __ATOMIC_ACQUIRE);
    if (!lock) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool on = onair_on;
    xSemaphoreGive(lock);
    return on;
}

// console_usb_hold's question: what makes the console's destructive commands USB-only now
// Lock-free (#420): the update server asks it per firmware request, and with the list off that costs no lock. seq_cst:
// the server's load in the Dekker pair of ota_hold.h
static char const *onair_hold(void) {
    return __atomic_load_n(&onair_on, __ATOMIC_SEQ_CST) ? "the on-air allow list (fw onair)" : NULL;
}

void mesh_onair_init(void) {
    static char text[ONAIR_TEXT_MAX]; // boot, one task
    if (onair_lock) {
        return;
    }
    app_settings_get_str(meshcore_ctx(), KEY_LIST, text, sizeof(text), "");
    onair_damaged = !onair_decode(text, &onair_list);
    onair_on      = app_settings_get_u8(meshcore_ctx(), KEY_ON, 0) != 0;
    __atomic_store_n(&onair_lock, xSemaphoreCreateMutexStatic(&onair_lock_mem), __ATOMIC_RELEASE);
    console_usb_hold_bind(onair_hold); // the console's factory reset would erase the setting: USB only while it is on
    if (onair_damaged) {
        ESP_LOGW(TAG, "on-air allow list: the saved list is damaged, it is empty now (fw onair)");
    }
    if (onair_on) {
        ESP_LOGI(TAG, "on-air allow list on: console sends only to its %u entries", (unsigned)onair_list.n);
    }
}

char const *mesh_onair_refusal(bool from_console, onair_tx_t tx, char const *channel, uint8_t const *key_id,
                               uint8_t const *prefix) {
    if (!from_console || !__atomic_load_n(&onair_lock, __ATOMIC_ACQUIRE)) {
        return NULL; // a screen's send never reads it; the lock comes before the first console command
    }
    xSemaphoreTake(onair_lock, portMAX_DELAY);
    bool ok = mesh_onair_allows(&onair_list, onair_on, true, tx, channel, key_id, prefix);
    xSemaphoreGive(onair_lock);
    if (ok) {
        return NULL;
    }
    if (console_origin() == CONSOLE_ORIGIN_UNKNOWN) {
        return ONAIR_REFUSED_POLL; // the mesh task's: the buffer is the console runs'
    }
    onair_refused_text(tx, channel, prefix, onair_refused, sizeof(onair_refused));
    return onair_refused;
}

static void onair_status(void) {
    onair_list_t list;
    xSemaphoreTake(onair_lock, portMAX_DELAY);
    list    = onair_list;
    bool on = onair_on, damaged = onair_damaged;
    xSemaphoreGive(onair_lock);
    console_reply("on-air allow list %s: %s | %u of %d entries%s", on ? "on" : "off",
                  on ? "console sends go only to the list" : "console sends are not limited", (unsigned)list.n,
                  ONAIR_MAX, damaged ? " (the saved list was damaged)" : "");
    for (size_t i = 0; i < list.n; i++) {
        char line[ONAIR_NAME_MAX + 16];
        onair_entry_text(&list.e[i], line, sizeof(line));
        console_reply("%s", line);
    }
}

// Sets the live flag (seq_cst: onair_hold reads it without the lock, ota_hold.h) and returns what it was
static bool onair_set(bool on) {
    xSemaphoreTake(onair_lock, portMAX_DELAY);
    bool was = onair_on;
    __atomic_store_n(&onair_on, on, __ATOMIC_SEQ_CST);
    xSemaphoreGive(onair_lock);
    return was;
}

static void onair_switch(bool on) {
    onair_set(on);
    xSemaphoreTake(onair_lock, portMAX_DELAY);
    size_t n = onair_list.n;
    xSemaphoreGive(onair_lock);
    bool saved = app_settings_set_u8(meshcore_ctx(), KEY_ON, on ? 1 : 0);
    console_reply("on-air allow list %s (%s)%s", on ? "on" : "off", saved ? "saved" : "not saved, this boot only",
                  on && !n ? ": the list is empty, every console send is refused" : "");
}

// Not while another image could run without asking (#420): it may not keep the list. From every origin, USB too. The
// list's side of the Dekker pair (ota_hold.h): store on first, then load what is pending; pending: back off again.
static void onair_switch_on(void) {
    char const *pending = onair_set(true) ? NULL : console_boot_pending();
    if (pending) {
        onair_set(false);
        console_reply("on-air allow list not switched on: %s", pending);
        return;
    }
    onair_switch(true);
}

static char const *result_text(onair_result_t r) {
    switch (r) {
    case ONAIR_OK: return "done";
    case ONAIR_BAD: return "not a channel name nor 12 to 64 hex digits of a key";
    case ONAIR_FULL: return "the list is full (fw onair remove <entry>)";
    case ONAIR_DUPLICATE: return "already on the list";
    case ONAIR_NOT_FOUND: return "not on the list";
    case ONAIR_REKEYED: return "re-keyed";
    }
    return "?";
}

// "allow": the entry, a channel bound to the key of the channel its name resolves to now (stored name, key id); false
// (said why) when it does not parse or no channel of that name is on the badge
static bool allow_entry(char const *target, onair_entry_t *e) {
    onair_result_t r = onair_entry_parse(target, e);
    if (r != ONAIR_OK) {
        console_reply("on-air allow list not changed: %s", result_text(r));
        return false;
    }
    if (e->kind != ONAIR_CHANNEL) {
        return true;
    }
    size_t found = mesh_channel_key_id(e->name, true, e->name, sizeof(e->name), e->key_id);
    if (found != 1) {
        console_reply("on-air allow list not changed: %s", found == 0 ? "no channel of that name on the badge now (fw "
                                                                      "channels)" : "MeshCore is not running or busy");
        return false;
    }
    return true;
}

// allow, remove, clear: the change and its text under the lock, the write after it (console runs keep them in order)
static void onair_change(onair_op_t op, char const *target) {
    static char    text[ONAIR_TEXT_MAX]; // console runs only, one at a time
    onair_entry_t  e = {0};
    onair_result_t r = ONAIR_OK;
    if (op == ONAIR_OP_ALLOW && !allow_entry(target, &e)) {
        return;
    }
    xSemaphoreTake(onair_lock, portMAX_DELAY);
    if (op == ONAIR_OP_CLEAR) {
        memset(&onair_list, 0, sizeof(onair_list));
    } else {
        r = op == ONAIR_OP_ALLOW ? onair_add(&onair_list, &e) : onair_remove(&onair_list, target);
    }
    bool changed  = r == ONAIR_OK || r == ONAIR_REKEYED;
    bool fits     = onair_encode(&onair_list, text, sizeof(text));
    onair_damaged = onair_damaged && !changed;
    xSemaphoreGive(onair_lock);
    if (!changed) {
        console_reply("on-air allow list not changed: %s", result_text(r));
        return;
    }
    bool saved = fits && (text[0] ? app_settings_set_str(meshcore_ctx(), KEY_LIST, text)
                                  : app_settings_erase(meshcore_ctx(), KEY_LIST));
    console_reply("on-air allow list changed (%s)%s", saved ? "saved" : "not saved, this boot only",
                  r == ONAIR_REKEYED ? ": the channel has another key now, only the new one is listed" : "");
}

void mesh_onair_console(char const *args) {
    char const *rest;
    onair_op_t  op = onair_op_parse(args, &rest);
    if (!__atomic_load_n(&onair_lock, __ATOMIC_ACQUIRE)) {
        console_reply("on-air allow list: not loaded yet");
        return;
    }
    // Switching it off and editing it: the USB console only, its rows in mesh_cmd_table.c, checked before this runs
    switch (op) {
    case ONAIR_OP_STATUS: onair_status(); break;
    case ONAIR_OP_ON: onair_switch_on(); break;
    case ONAIR_OP_OFF: onair_switch(false); break;
    case ONAIR_OP_ALLOW:
    case ONAIR_OP_REMOVE:
    case ONAIR_OP_CLEAR: onair_change(op, rest); break;
    case ONAIR_OP_USAGE:
        console_reply("onair [on | off | allow <channel|key digits> | remove <channel|key digits> | clear]");
        break;
    }
}
