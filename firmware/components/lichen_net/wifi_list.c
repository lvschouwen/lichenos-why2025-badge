#include "wifi_list.h"

#include "wifi_saved.h"


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
// Set once at boot (settings_bind_net) before the Wi-Fi service's task, console command or screen can run
static wifi_store_t const *store;

void wifi_list_bind_store(wifi_store_t const *s) {
    store = s;
}

// ---- the pair in use -----------------------------------------------------------------------------------------------

bool wifi_cred_load(char ssid[33], char pass[64]) {
    ssid[0] = '\0';
    pass[0] = '\0';
    if (!store) {
        return false;
    }
    store->get_str("wifi_ssid", ssid, 33, "");
    return ssid[0] && store->secret_get("wifi_pass", pass, 64); // an SSID is 1-32 bytes: empty is "none saved"
}

// Writes the pair in use (NULL ssid: removes it). A failure half way is the caller's to undo.
static bool set_active(char const *ssid, char const *pass) {
    if (!store) {
        return false;
    }
    if (!ssid) {
        bool a = store->erase("wifi_ssid");
        return store->erase("wifi_pass") && a;
    }
    return store->set_str("wifi_ssid", ssid) && store->secret_set("wifi_pass", pass);
}

// ---- the saved list (#160) ---------------------------------------------------------------------------------------

static char *slot_key(char key[10], char kind, int i) {
    key[0] = 'w'; key[1] = 'i'; key[2] = 'f'; key[3] = 'i'; key[4] = '_';
    key[5] = kind;
    key[6] = (char)('0' + i);
    key[7] = '\0';
    return key;
}

// SSIDs as plain settings, passwords as secrets; slots past the list's end are erased
static bool saved_store(wifi_saved_t const *l) {
    if (!store) {
        return false;
    }
    // Every slot whatever the result: a failed write or erase never leaves the slots after it untouched (a clear
    // that stopped half way would keep old passwords)
    bool ok = true;
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        char ks[10], kp[10];
        slot_key(ks, 's', i);
        slot_key(kp, 'p', i);
        bool done;
        if ((size_t)i < l->n) {
            done = store->set_str(ks, l->e[i].ssid);
            done = store->secret_set(kp, l->e[i].pass) && done;
        } else {
            done = store->erase(ks);
            done = store->erase(kp) && done;
        }
        ok = ok && done;
    }
    return ok;
}

bool wifi_saved_load(wifi_saved_t *l) {
    memset(l, 0, sizeof(*l));
    if (!store) {
        return false;
    }
    for (int i = 0; i < WIFI_SAVED_MAX; i++) {
        char ks[10], kp[10];
        store->get_str(slot_key(ks, 's', i), l->e[l->n].ssid, sizeof(l->e[0].ssid), "");
        if (l->e[l->n].ssid[0] &&
            store->secret_get(slot_key(kp, 'p', i), l->e[l->n].pass, sizeof(l->e[0].pass)) &&
            wifi_cred_valid(l->e[l->n].ssid, l->e[l->n].pass)) {
            l->n++;
        } else {
            memset(&l->e[l->n], 0, sizeof(l->e[0])); // a half-read slot leaves nothing behind
        }
    }
    return true;
}

size_t wifi_saved_names_with(wifi_lock_t const *lock, char (*out)[33], size_t max) {
    wifi_saved_t l;
    if (lock) {
        lock->take();
    }
    wifi_saved_load(&l);
    if (lock) {
        lock->give();
    }
    size_t n = l.n < max ? l.n : max;
    for (size_t i = 0; i < n; i++) {
        memcpy(out[i], l.e[i].ssid, sizeof(out[i]));
    }
    wifi_saved_wipe(&l); // the copy of the passwords does not outlive the call
    return n;
}

// The pair just joined goes to the front of the list. Best effort: the pair in use is already safe in wifi_ssid.
static void saved_remember(wifi_saved_t *l, char const *ssid, char const *pass) {
    if (wifi_saved_add(l, ssid, pass)) {
        saved_store(l);
    }
    wifi_saved_wipe(l);
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
    bool ok = set_active(ssid, pass);
    if (!ok) {
        set_active(had ? old_ssid : NULL, old_pass);
    }
    memset(old_pass, 0, sizeof(old_pass));
    if (ok) {
        generation++;
        saved_remember(&list, ssid, pass);
    } else {
        wifi_saved_wipe(&list);
    }
    return ok;
}

bool wifi_cred_clear(void) {
    wifi_saved_t none   = {0};
    bool         active = set_active(NULL, ""); // both run: a failed erase of one never keeps the others
    bool         list   = saved_store(&none);
    generation++;
    return active && list;
}

bool wifi_driver_wiped(void) {
    return !store || store->get_u8("wifi_drv_wiped", 0) != 0;
}

bool wifi_driver_wiped_set(bool wiped) {
    return store && store->set_u8("wifi_drv_wiped", wiped ? 1 : 0);
}
