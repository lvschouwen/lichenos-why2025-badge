#include "wifi_list.h"

#include "wifi_saved.h"

#include "fw_console.h"
#include "nvs.h"

#include <string.h>

int wifi_bars(int8_t rssi) {
    return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
}

size_t wifi_list_clean(wifi_ap_t *aps, size_t n) {
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (aps[i].ssid[0] == '\0') {
            continue;
        }
        size_t j = 0;
        while (j < kept && strcmp(aps[j].ssid, aps[i].ssid) != 0) {
            j++;
        }
        if (j < kept) {
            // Any secured entry makes the network "locked": an open twin must not skip the password
            bool open = aps[j].open && aps[i].open;
            if (aps[i].rssi > aps[j].rssi) {
                aps[j] = aps[i];
            }
            aps[j].open = open;
        } else {
            aps[kept++] = aps[i];
        }
    }
    // Insertion sort: stable and a few dozen entries at most
    for (size_t i = 1; i < kept; i++) {
        wifi_ap_t ap = aps[i];
        size_t    j  = i;
        while (j > 0 && aps[j - 1].rssi < ap.rssi) {
            aps[j] = aps[j - 1];
            j--;
        }
        aps[j] = ap;
    }
    return kept;
}

// Length of a strictly valid UTF-8 sequence (no overlongs, surrogates or above U+10FFFF) at s that is also safe to
// show (no C1 controls, no bidi or format controls), or 0
static size_t utf8_len(unsigned char const *s) {
    unsigned char c = s[0];
    size_t        n = c >= 0xC2 && c <= 0xDF ? 2 : (c & 0xF0) == 0xE0 ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (n == 0) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0; // also stops at the NUL
        }
    }
    if ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] > 0x9F) || (c == 0xF0 && s[1] < 0x90) ||
        (c == 0xF4 && s[1] > 0x8F)) {
        return 0;
    }
    uint32_t cp = n == 2 ? ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu)
                : n == 3 ? ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu)
                         : ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
    if ((cp >= 0x80 && cp <= 0x9F) || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2066 && cp <= 0x2069)) {
        return 0;
    }
    return n;
}

size_t wifi_ssid_sanitize(char const *in, char *out, size_t cap) {
    static char const hex[] = "0123456789abcdef";
    size_t            o     = 0;
    if (cap == 0) {
        return 0;
    }
    for (unsigned char const *p = (unsigned char const *)in; *p;) {
        size_t keep = *p >= 0x80 ? utf8_len(p) : (*p >= 0x20 && *p != 0x7F ? 1 : 0);
        if (keep) {
            if (o + keep + 1 > cap) {
                break;
            }
            for (size_t i = 0; i < keep; i++) {
                out[o++] = (char)p[i];
            }
            p += keep;
        } else {
            if (o + 4 + 1 > cap) {
                break;
            }
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
            p++;
        }
    }
    out[o] = '\0';
    return o;
}

bool wifi_cred_valid(char const *ssid, char const *pass) {
    if (!ssid || !pass) {
        return false;
    }
    size_t s = strlen(ssid);
    size_t p = strlen(pass);
    return s >= 1 && s <= 32 && (p == 0 || (p >= 8 && p <= 63));
}

static uint32_t generation;

// ---- the saved list (#160) ---------------------------------------------------------------------------------------

static char *slot_key(char key[10], char kind, int i) {
    key[0] = 'w'; key[1] = 'i'; key[2] = 'f'; key[3] = 'i'; key[4] = '_';
    key[5] = kind;
    key[6] = (char)('0' + i);
    key[7] = '\0';
    return key;
}

static bool saved_store(wifi_saved_t const *l) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = ESP_OK;
    for (int i = 0; i < WIFI_SAVED_MAX && err == ESP_OK; i++) {
        char ks[10], kp[10];
        slot_key(ks, 's', i);
        slot_key(kp, 'p', i);
        if ((size_t)i < l->n) {
            err = nvs_set_str(nvs, ks, l->e[i].ssid);
            if (err == ESP_OK) {
                err = nvs_set_str(nvs, kp, l->e[i].pass);
            }
        } else {
            nvs_erase_key(nvs, ks);
            nvs_erase_key(nvs, kp);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

bool wifi_saved_load(wifi_saved_t *l) {
    memset(l, 0, sizeof(*l));
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        char   ks[10], kp[10];
        size_t sl = sizeof(l->e[0].ssid), pl = sizeof(l->e[0].pass);
        if (nvs_get_str(nvs, slot_key(ks, 's', i), l->e[l->n].ssid, &sl) == ESP_OK &&
            nvs_get_str(nvs, slot_key(kp, 'p', i), l->e[l->n].pass, &pl) == ESP_OK &&
            wifi_cred_valid(l->e[l->n].ssid, l->e[l->n].pass)) {
            l->n++;
        }
    }
    nvs_close(nvs);
    if (l->n == 0) { // first boot after #160: the single saved pair becomes entry 1
        char s[33], p[64];
        if (wifi_cred_load(s, p) && wifi_saved_migrate(l, s, p)) {
            saved_store(l);
        }
        memset(p, 0, sizeof(p));
    }
    return true;
}

// The pair just joined goes to the front of the list (l was loaded before wifi_ssid changed, so a pre-#160 pair has
// been migrated by then). Best effort: the pair in use is already safe in wifi_ssid.
static void saved_remember(wifi_saved_t *l, char const *ssid, char const *pass) {
    if (wifi_saved_add(l, ssid, pass)) {
        saved_store(l);
    }
    wifi_saved_wipe(l);
}

static bool set_active(char const *ssid, char const *pass) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err;
    if (ssid) {
        err = nvs_set_str(nvs, "wifi_ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, "wifi_pass", pass);
        }
    } else {
        nvs_erase_key(nvs, "wifi_ssid");
        nvs_erase_key(nvs, "wifi_pass");
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

bool wifi_cred_forget(char const *ssid, bool *was_active, bool *left) {
    wifi_saved_t l;
    wifi_saved_load(&l);
    char act[33] = "", pass[64];
    wifi_cred_load(act, pass);
    bool active = strcmp(act, ssid) == 0;
    bool ok     = wifi_saved_forget(&l, ssid) && saved_store(&l);
    // If the pair in use cannot be replaced, wifi_ssid still names the forgotten network: that is a failure, and the
    // caller still has to drop it from the driver (was_active)
    bool swapped = !(ok && active) || set_active(l.n ? l.e[0].ssid : NULL, l.n ? l.e[0].pass : "");
    if (ok) {
        generation++;
    }
    if (was_active) {
        *was_active = ok && active;
    }
    ok = ok && swapped;
    if (left) {
        *left = l.n > 0;
    }
    wifi_saved_wipe(&l);
    memset(pass, 0, sizeof(pass));
    return ok;
}

bool wifi_cred_activate(char const *ssid) {
    wifi_saved_t l;
    wifi_saved_load(&l);
    int  at = wifi_saved_find(&l, ssid);
    bool ok = at >= 0;
    if (ok && at > 0) {
        char pass[64];
        strcpy(pass, l.e[at].pass);
        ok = wifi_saved_add(&l, ssid, pass) && saved_store(&l) && set_active(ssid, pass);
        memset(pass, 0, sizeof(pass));
        generation++;
    }
    wifi_saved_wipe(&l);
    return ok;
}

uint32_t wifi_cred_generation(void) {
    return generation;
}

// Both keys or neither: a failure half way puts the previous pair back (or removes what was written)
bool wifi_cred_save(char const *ssid, char const *pass) {
    if (!wifi_cred_valid(ssid, pass)) {
        return false;
    }
    char old_ssid[33], old_pass[64];
    bool had = wifi_cred_load(old_ssid, old_pass);
    wifi_saved_t list;
    wifi_saved_load(&list);
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        memset(old_pass, 0, sizeof(old_pass));
        wifi_saved_wipe(&list);
        return false;
    }
    esp_err_t err = nvs_set_str(nvs, "wifi_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "wifi_pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        if (had) {
            nvs_set_str(nvs, "wifi_ssid", old_ssid);
            nvs_set_str(nvs, "wifi_pass", old_pass);
        } else {
            nvs_erase_key(nvs, "wifi_ssid");
            nvs_erase_key(nvs, "wifi_pass");
        }
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    memset(old_pass, 0, sizeof(old_pass));
    if (err == ESP_OK) {
        generation++;
        saved_remember(&list, ssid, pass);
    } else {
        wifi_saved_wipe(&list);
    }
    return err == ESP_OK;
}

bool wifi_cred_clear(void) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    nvs_erase_key(nvs, "wifi_ssid");
    nvs_erase_key(nvs, "wifi_pass");
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        char key[10];
        nvs_erase_key(nvs, slot_key(key, 's', i));
        nvs_erase_key(nvs, slot_key(key, 'p', i));
    }
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    generation++;
    return err == ESP_OK;
}

bool wifi_driver_wiped(void) {
    nvs_handle_t nvs;
    uint8_t      done = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return true;
    }
    bool wiped = nvs_get_u8(nvs, "wifi_drv_wiped", &done) == ESP_OK && done;
    nvs_close(nvs);
    return wiped;
}

void wifi_driver_wiped_set(void) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u8(nvs, "wifi_drv_wiped", 1);
    nvs_commit(nvs);
    nvs_close(nvs);
}

bool wifi_cred_load(char ssid[33], char pass[64]) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t ssid_len = 33;
    size_t pass_len = 64;
    bool   ok       = nvs_get_str(nvs, "wifi_ssid", ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(nvs, "wifi_pass", pass, &pass_len) == ESP_OK;
    nvs_close(nvs);
    return ok;
}
