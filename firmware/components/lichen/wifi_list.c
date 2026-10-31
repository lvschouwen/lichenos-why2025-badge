#include "wifi_list.h"

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
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        memset(old_pass, 0, sizeof(old_pass));
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
    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    generation++;
    return err == ESP_OK;
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
