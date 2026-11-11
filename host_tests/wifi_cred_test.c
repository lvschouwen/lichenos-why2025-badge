#define _GNU_SOURCE
// Host test: Wi-Fi credential rules and the round trip through the settings service, passwords as secrets (#66, #235)
#include "nvs.h"
#include "settings_secret.h"
#include "wifi_list.h"
#include "wifi_saved.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int  lock_takes, lock_gives, held;
static bool lock_busy;       // take times out (c6wifi holds the lock across its RPC)
static int  held_at_rpc = -1; // the lock count seen inside the apply hook
static int  allocs, releases, dirty_release;
static bool lock_take(uint32_t wait_ms) {
    (void)wait_ms;
    if (lock_busy) {
        return false; // returns at once: a test that blocked would hang here
    }
    lock_takes++;
    held++;
    return true;
}
static void lock_give(void) { lock_gives++; held--; }
static bool   alloc_fail;
static size_t blk_size;
static void *blk_alloc(size_t n) {
    if (alloc_fail) {
        return NULL;
    }
    allocs++;
    blk_size = n;
    void *p  = malloc(n);
    if (p) {
        memset(p, 0xAA, n); // a block that is never wiped stays visibly dirty
    }
    return p;
}
static void blk_release(void *p) {
    // wipe-before-free: the whole block must be zero again, not only its start
    unsigned char const *b = p;
    for (size_t i = 0; i < blk_size; i++) {
        if (b[i] != 0) {
            dirty_release++;
            break;
        }
    }
    releases++;
    free(p);
}
static wifi_lock_t const test_lock = {.take = lock_take, .give = lock_give, .alloc = blk_alloc, .release = blk_release};

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char const *text(char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)('a' + i % 26);
    }
    buf[len] = '\0';
    return buf;
}


// The settings service as settings_bind_net() binds it: settings_secret.c (the real one, over the NVS stub) for the
// passwords, and the plain calls as settings_store.c makes them, refusing a secret key the same way
static void plain_get_str(char const *key, char *out, size_t cap, char const *def) {
    nvs_handle_t nvs;
    size_t       len = cap;
    if (!settings_key_secret("badge_fw", key) && nvs_open("badge_fw", NVS_READONLY, &nvs) == ESP_OK &&
        nvs_get_str(nvs, key, out, &len) == ESP_OK) {
        return;
    }
    snprintf(out, cap, "%s", def);
}

static bool plain_set_str(char const *key, char const *value) {
    nvs_handle_t nvs;
    return !settings_key_secret("badge_fw", key) && nvs_open("badge_fw", NVS_READWRITE, &nvs) == ESP_OK &&
           nvs_set_str(nvs, key, value) == ESP_OK;
}

static int erase_fail_in; // the nth plain erase from now on fails (1 = the next one); 0 switches it off

static bool plain_erase(char const *key) {
    if (erase_fail_in > 0 && --erase_fail_in == 0) {
        return false;
    }
    nvs_handle_t nvs;
    nvs_open("badge_fw", NVS_READWRITE, &nvs);
    esp_err_t err = nvs_erase_key(nvs, key);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

static uint8_t plain_get_u8(char const *key, uint8_t def) {
    nvs_handle_t nvs;
    uint8_t      v = def;
    nvs_open("badge_fw", NVS_READONLY, &nvs);
    return nvs_get_u8(nvs, key, &v) == ESP_OK ? v : def;
}

static bool plain_set_u8(char const *key, uint8_t value) {
    nvs_handle_t nvs;
    nvs_open("badge_fw", NVS_READWRITE, &nvs);
    return nvs_set_u8(nvs, key, value) == ESP_OK;
}

static wifi_store_t const test_store = {
    .get_str    = plain_get_str,
    .set_str    = plain_set_str,
    .erase      = plain_erase,
    .get_u8     = plain_get_u8,
    .set_u8     = plain_set_u8,
    .secret_get = settings_secret_get,
    .secret_set = settings_secret_set,
};

// #235: before the settings service binds its store nothing is read or written
static void unbound(void) {
    char         s[33], p[64];
    wifi_saved_t l;
    CHECK(!wifi_cred_load(s, p) && s[0] == '\0' && p[0] == '\0');
    CHECK(!wifi_saved_load(&l) && l.n == 0);
    CHECK(!wifi_cred_save("net", "password"));
    CHECK(!wifi_cred_clear());
    CHECK(!wifi_driver_wipe_pending() && !wifi_driver_wipe_done());
}

// #235: the passwords are secrets (settings_secret.c), the SSIDs plain settings under their old names, so what an
// earlier firmware saved still reads; v2 migrates nothing, so a lone pair in use does not become a list entry
static void secrets_and_defaults(void) {
    char         s[33], p[64], v[64];
    wifi_saved_t l;
    CHECK(wifi_cred_clear());
    CHECK(wifi_saved_load(&l) && l.n == 0); // the empty list is the default
    CHECK(wifi_cred_save("homenet", "homepassword"));
    plain_get_str("wifi_pass", v, sizeof(v), "-");
    CHECK(strcmp(v, "-") == 0); // the plain path never hands a password out
    plain_get_str("wifi_p0", v, sizeof(v), "-");
    CHECK(strcmp(v, "-") == 0);
    CHECK(settings_secret_get("wifi_pass", v, sizeof(v)) && strcmp(v, "homepassword") == 0);
    CHECK(settings_secret_get("wifi_p0", v, sizeof(v)) && strcmp(v, "homepassword") == 0);
    plain_get_str("wifi_ssid", v, sizeof(v), "");
    CHECK(strcmp(v, "homenet") == 0);
    CHECK(!settings_secret_get("wifi_ssid", v, sizeof(v)) && v[0] == '\0'); // the secret API is for secrets only
    CHECK(!settings_secret_set("tz", "UTC0"));
    CHECK(!plain_set_str("wifi_pass", "leak"));

    // Saved by hand as an older firmware would leave it: the pair in use reads, the list stays empty
    CHECK(wifi_cred_clear());
    CHECK(plain_set_str("wifi_ssid", "oldnet") && settings_secret_set("wifi_pass", "oldpassword"));
    CHECK(wifi_cred_load(s, p) && strcmp(s, "oldnet") == 0 && strcmp(p, "oldpassword") == 0);
    CHECK(wifi_saved_load(&l) && l.n == 0);
    CHECK(wifi_saved_load(&l) && l.n == 0); // and loading never writes one in
    // An SSID without its password is no pair
    CHECK(plain_erase("wifi_pass"));
    CHECK(!wifi_cred_load(s, p));
}

// #160: the saved list, through the bound store
static void saved_networks(void) {
    char s[33], p[64];
    wifi_saved_t l;
    CHECK(wifi_cred_clear());
    CHECK(wifi_cred_save("homenet", "homepassword"));
    CHECK(wifi_saved_load(&l) && l.n == 1 && strcmp(l.e[0].ssid, "homenet") == 0 && strcmp(l.e[0].pass, "homepassword") == 0);

    // Joining a second network keeps the first; the new one is the pair in use and first in the list
    CHECK(wifi_cred_save("campnet", "camppassword"));
    CHECK(wifi_saved_load(&l) && l.n == 2 && strcmp(l.e[0].ssid, "campnet") == 0 && strcmp(l.e[1].ssid, "homenet") == 0);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "campnet") == 0);

    // Forgetting the pair in use makes the next one the pair in use; the last one leaves nothing
    bool active = false, left = false;
    CHECK(wifi_cred_forget("campnet", &active, &left) && active && left);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "homenet") == 0 && strcmp(p, "homepassword") == 0);
    CHECK(!wifi_cred_forget("campnet", &active, &left)); // already gone
    CHECK(wifi_cred_forget("homenet", &active, &left) && active && !left);
    CHECK(!wifi_cred_load(s, p));
    CHECK(wifi_saved_load(&l) && l.n == 0);

    // Forgetting one that is not in use leaves the pair in use alone; activate moves a saved one to the front
    CHECK(wifi_cred_save("a-net", "passwordaaa"));
    CHECK(wifi_cred_save("b-net", "passwordbbb"));
    CHECK(wifi_cred_save("c-net", "passwordccc"));
    CHECK(wifi_cred_forget("a-net", &active, &left) && !active && left);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "c-net") == 0);
    CHECK(wifi_cred_activate("b-net") && wifi_cred_load(s, p) && strcmp(s, "b-net") == 0 && strcmp(p, "passwordbbb") == 0);
    CHECK(wifi_saved_load(&l) && l.n == 2 && strcmp(l.e[0].ssid, "b-net") == 0);
    CHECK(!wifi_cred_activate("nope"));

    // Forgetting the pair in use when the pair cannot be replaced: failure, but the caller still learns it was in use
    CHECK(wifi_cred_clear());
    CHECK(wifi_cred_save("x-net", "passwordxxx"));
    CHECK(wifi_cred_save("y-net", "passwordyyy"));
    nvs_stub_fail_set(3); // two writes for the list, then wifi_ssid
    CHECK(!wifi_cred_forget("y-net", &active, &left) && active);
    nvs_stub_fail_set(0);

    // A clear whose erase fails on one slot still erases every slot after it (wifi_ssid, wifi_pass, then wifi_s0 fails)
    CHECK(wifi_cred_save("x-net", "passwordxxx"));
    CHECK(wifi_cred_save("z-net", "passwordzzz"));
    erase_fail_in = 3;
    CHECK(!wifi_cred_clear());
    erase_fail_in = 0;
    char v[64];
    CHECK(!settings_secret_get("wifi_p0", v, sizeof(v)) && !settings_secret_get("wifi_p1", v, sizeof(v)));

    // The factory reset and "fw wifi --clear" remove every saved network, not only the one in use
    CHECK(wifi_cred_clear());
    CHECK(wifi_saved_load(&l) && l.n == 0 && !wifi_cred_load(s, p));
}

// #240: the Wi-Fi screen gets the SSIDs only, read under the list lock; no password byte reaches its buffer
static void names_only(void) {
    char out[WIFI_SAVED_MAX][33];
    memset(out, 0, sizeof(out));
    CHECK(wifi_cred_clear());
    CHECK(wifi_cred_save("homenet", "uniquepass1") && wifi_cred_save("campnet", "uniquepass2"));
    size_t n = wifi_saved_names_with(&test_lock, 50, out, WIFI_SAVED_MAX);
    CHECK(n == 2 && strcmp(out[0], "campnet") == 0 && strcmp(out[1], "homenet") == 0);
    CHECK(lock_takes == 1 && lock_gives == 1 && held == 0);
    CHECK(memmem(out, sizeof(out), "uniquepass", 10) == NULL);
    CHECK(wifi_saved_names_with(&test_lock, 50, out, 1) == 1 && lock_takes == 2 && lock_gives == 2);
    // #249: the copy is on the heap and wiped before it is freed
    CHECK(allocs == releases && dirty_release == 0);
}

// #249: a busy lock returns at once with BUSY, the copy is released, nothing was read
static void names_busy(void) {
    char out[WIFI_SAVED_MAX][33];
    int  takes = lock_takes, a = allocs, r = releases;
    int  d = dirty_release;
    lock_busy = true;
    CHECK(wifi_saved_names_with(&test_lock, 50, out, WIFI_SAVED_MAX) == WIFI_SAVED_BUSY);
    lock_busy = false;
    CHECK(dirty_release == d + 1); // the block was never filled, so it is still the 0xAA pattern: nothing was wiped
    dirty_release = d;
    alloc_fail = true; // no memory: busy as well, and the lock is never taken
    CHECK(wifi_saved_names_with(&test_lock, 50, out, WIFI_SAVED_MAX) == WIFI_SAVED_BUSY && lock_takes == takes);
    alloc_fail = false;
    CHECK(lock_takes == takes && held == 0 && allocs == a + 1 && releases == r + 1);
}

static int  committed_held = -1;
static char committed_name[33];
static void committed_hook(char const *name) {
    committed_held = held;
    snprintf(committed_name, sizeof(committed_name), "%s", name);
}

static bool rpc_hook(char const *ssid, char const *pass) {
    held_at_rpc = held;
    return strcmp(ssid, "homenet") == 0 && strcmp(pass, "uniquepass1") == 0;
}

// #249: the C6 RPC (apply) runs with the list lock released; the lock is taken again afterwards to re-check
static void apply_outside_lock(void) {
    CHECK(wifi_cred_clear());
    CHECK(wifi_cred_save("homenet", "uniquepass1") && wifi_cred_save("campnet", "uniquepass2"));
    int takes = lock_takes;
    held_at_rpc = -1;
    CHECK(wifi_saved_apply_with(&test_lock, "homenet", rpc_hook, committed_hook) == WIFI_APPLY_APPLIED);
    CHECK(held_at_rpc == 0 && held == 0 && lock_takes == takes + 2 && lock_takes == lock_gives);
    CHECK(committed_held == 1 && strcmp(committed_name, "homenet") == 0); // recorded under the lock (#249 TOCTOU)
    held_at_rpc = -1;
    CHECK(wifi_saved_apply_with(&test_lock, "nonet", rpc_hook, committed_hook) == WIFI_APPLY_NOT_SAVED && held_at_rpc == -1);
    CHECK(wifi_saved_apply_with(&test_lock, "campnet", rpc_hook, committed_hook) == WIFI_APPLY_FAILED && held_at_rpc == 0);
    CHECK(dirty_release == 0 && allocs == releases);
}

static bool forgetting_rpc(char const *ssid, char const *pass) {
    bool was, left;
    (void)ssid; (void)pass;
    held_at_rpc = held;
    return wifi_cred_forget("homenet", &was, &left); // a Forget lands during the RPC
}

static void apply_forgotten(void) {
    CHECK(wifi_cred_clear());
    CHECK(wifi_cred_save("homenet", "uniquepass1") && wifi_cred_save("campnet", "uniquepass2"));
    committed_held = -1;
    CHECK(wifi_saved_apply_with(&test_lock, "homenet", forgetting_rpc, committed_hook) == WIFI_APPLY_FORGOTTEN && held_at_rpc == 0);
    CHECK(committed_held == -1); // a forgotten pick is never recorded as the driver's network
    alloc_fail = true;
    held_at_rpc = -1;
    CHECK(wifi_saved_apply_with(&test_lock, "campnet", rpc_hook, committed_hook) == WIFI_APPLY_NOT_SAVED && held_at_rpc == -1);
    alloc_fail = false;
    // Forget without memory fails and leaves the list alone
    bool was = false, left = false;
    wifi_list_bind_alloc(NULL, NULL);
    CHECK(!wifi_cred_forget("campnet", &was, &left));
    wifi_list_bind_alloc(blk_alloc, blk_release);
    CHECK(wifi_cred_forget("campnet", &was, &left) && left == false);
}

int main(void) {
    unbound();
    wifi_list_bind_store(&test_store);
    wifi_list_bind_alloc(blk_alloc, blk_release);
    char s[40], p[80];
    CHECK(!wifi_cred_valid(text(s, 0), "password"));
    CHECK(wifi_cred_valid(text(s, 1), "password"));
    CHECK(wifi_cred_valid(text(s, 32), "password"));
    CHECK(!wifi_cred_valid(text(s, 33), "password"));
    CHECK(wifi_cred_valid("net", text(p, 0))); // open network
    CHECK(!wifi_cred_valid("net", text(p, 7)));
    CHECK(wifi_cred_valid("net", text(p, 8)));
    CHECK(wifi_cred_valid("net", text(p, 63)));
    CHECK(!wifi_cred_valid("net", text(p, 64)));
    CHECK(!wifi_cred_valid(NULL, "password") && !wifi_cred_valid("net", NULL));

    // 63 bytes with a space, a double quote and a backslash survive the save and load byte for byte
    char pass[64];
    memset(pass, 'x', 63);
    pass[63] = '\0';
    pass[5]  = ' ';
    pass[20] = '"';
    pass[40] = '\\';
    pass[62] = ' ';
    uint32_t gen = wifi_cred_generation();
    CHECK(wifi_cred_save("my net", pass));
    CHECK(wifi_cred_generation() == gen + 1);
    char ssid_back[33], pass_back[64];
    CHECK(wifi_cred_load(ssid_back, pass_back));
    CHECK(strcmp(ssid_back, "my net") == 0);
    CHECK(memcmp(pass_back, pass, 64) == 0);

    // Open network: empty password is stored and read back as empty
    CHECK(wifi_cred_save("cafe", ""));
    CHECK(wifi_cred_load(ssid_back, pass_back) && strcmp(ssid_back, "cafe") == 0 && pass_back[0] == '\0');

    // Invalid input is refused and leaves the saved pair alone
    CHECK(!wifi_cred_save("cafe", "short"));
    CHECK(wifi_cred_load(ssid_back, pass_back) && strcmp(ssid_back, "cafe") == 0);

    // A write that fails half way keeps the old pair (no new SSID with the old password) and does not count as a change
    CHECK(wifi_cred_save("old", "oldpassword"));
    gen = wifi_cred_generation();
    nvs_stub_fail_set(2); // the password write
    CHECK(!wifi_cred_save("new", "newpassword"));
    CHECK(wifi_cred_load(ssid_back, pass_back) && strcmp(ssid_back, "old") == 0 && strcmp(pass_back, "oldpassword") == 0);
    CHECK(wifi_cred_generation() == gen);
    // With nothing saved before, a failed save leaves nothing behind
    CHECK(wifi_cred_clear());
    nvs_stub_fail_set(2);
    CHECK(!wifi_cred_save("new", "newpassword"));
    CHECK(!wifi_cred_load(ssid_back, pass_back));

    CHECK(wifi_cred_clear());
    CHECK(!wifi_cred_load(ssid_back, pass_back));

    // A factory reset's order to clear the driver store (#342): pending until done, and a forget does not clear it
    CHECK(!wifi_driver_wipe_pending());
    CHECK(plain_set_u8("wifi_drv_wipe", 1) && wifi_driver_wipe_pending());
    CHECK(wifi_cred_clear() && wifi_driver_wipe_pending());
    CHECK(wifi_driver_wipe_done() && !wifi_driver_wipe_pending());

    secrets_and_defaults();
    saved_networks();
    names_only();
    names_busy();
    apply_outside_lock();
    apply_forgotten();

    if (failures) {
        return 1;
    }
    printf("wifi_cred_test: ok\n");
    return 0;
}
