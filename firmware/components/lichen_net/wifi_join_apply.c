#include "wifi_join_apply.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "fw_console.h"
#include "wifi_eap_drv.h"
#include "wifi_saved.h"
#include "wifi_service.h"

#include <stdatomic.h>
#include <string.h>

static char const TAG[] = "c6link"; // the log tag of the whole Wi-Fi service

// enterprise_on: at least one EAP call got through since the C6's driver came up (a C6 reset loses it), so a PSK network
// clears it. An old C6 fails the first call: it stays false and is never sent the clear RPCs. config_bad: see wifi_apply_blocked. eap_fail: the last reason (fw
// wifi shows it). eap_ca: the registry's wifi.eap_ca.
static atomic_bool enterprise_on;
static atomic_bool config_bad;
static atomic_int  eap_fail;
static atomic_int  eap_ca = WIFI_EAP_CA_NONE;

void wifi_apply_reset(void) {
    atomic_store(&enterprise_on, false);
    atomic_store(&config_bad, false);
}

bool wifi_apply_blocked(void) {
    return atomic_load(&config_bad);
}

void wifi_apply_clear_fail(void) {
    atomic_store(&eap_fail, WIFI_EAP_FAIL_NONE);
}

void wifi_apply_forgotten(void) {
    atomic_store(&eap_ca, WIFI_EAP_CA_NONE);
    atomic_store(&eap_fail, WIFI_EAP_FAIL_NONE);
}

wifi_eap_fail_t wifi_eap_last_fail(void) {
    return (wifi_eap_fail_t)atomic_load(&eap_fail);
}

wifi_eap_ca_state_t wifi_eap_ca(void) {
    return (wifi_eap_ca_state_t)atomic_load(&eap_ca);
}

typedef struct {
    wifi_eap_t e;
    char       rec[WIFI_EAP_BLOB_MAX];
} scratch_t; // ~0.7 KB: on the heap, never on the 5 KB c6wifi stack; wiped whole before it is released

static void failed(wifi_eap_fail_t why) {
    atomic_store(&eap_fail, (int)why);
    atomic_store(&eap_ca, WIFI_EAP_CA_NONE); // nothing was applied: wifi.eap_ca must not keep the previous network's state
    atomic_store(&config_bad, true);
    ESP_LOGW(TAG, "wifi: Enterprise join not started: %s", wifi_eap_fail_text(why));
}

static void scratch_free(scratch_t *sc, wifi_lock_t const *lock) {
    if (sc) {
        memset(sc, 0, sizeof(*sc));
        lock->release(sc);
    }
}

// 802.11k (radio measurement: neighbour reports) and 802.11v (BSS transition: the network steers the badge to a better
// AP of the same name, #495). The C6 is built with CONFIG_ESP_WIFI_11KV_SUPPORT; the join itself already takes the
// strongest AP (the esp_hosted set-config handler forces an all-channel scan sorted by signal).
static void sta_roam_flags(wifi_config_t *cfg) {
    cfg->sta.rm_enabled  = 1;
    cfg->sta.btm_enabled = 1;
}

// The station config alone; its own frame, so it is never live while the CA file is read
static esp_err_t set_sta_config(char const *s, char const *p, bool eap) {
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, s, strlen(s));
    if (eap) {
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE; // an open or PSK AP of that name is a trap
    } else {
        memcpy(cfg.sta.password, p, strlen(p));
        if (p[0]) {
            cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // a password was set: never join an open AP of that name
        }
    }
    sta_roam_flags(&cfg);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    return err;
}

static bool config_done(esp_err_t err, wifi_eap_t const *eap) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi config: %s", esp_err_to_name(err));
        return false;
    }
    atomic_store(&config_bad, false);
    atomic_store(&eap_ca, (int)wifi_eap_ca_state(eap));
    return true;
}

// An Enterprise network follows esp_eap_client.h: the credentials, then the station config, then enable. A refusal on the
// way clears what was set and puts the previous config back; config_bad then keeps everything from connecting.
static bool apply_eap(char const *s, wifi_eap_t const *eap, wifi_lock_t const *lock) {
    bool            touched;
    wifi_eap_fail_t why = wifi_eap_drv_set(eap, lock, &touched);
    atomic_store(&enterprise_on, wifi_enterprise_after(atomic_load(&enterprise_on), touched));
    wifi_config_t *prev = why == WIFI_EAP_FAIL_NONE ? lock->alloc(sizeof(*prev)) : NULL; // to put back, not on the stack
    bool           have_prev = prev && esp_wifi_get_config(WIFI_IF_STA, prev) == ESP_OK;
    esp_err_t      err = ESP_FAIL;
    if (why == WIFI_EAP_FAIL_NONE) {
        err = set_sta_config(s, "", true);
        why = err == ESP_OK ? wifi_eap_drv_enable() : wifi_eap_config_fail((int)err);
    }
    if (why != WIFI_EAP_FAIL_NONE) {
        unsigned undo = wifi_apply_unwind(touched, have_prev, err == ESP_OK);
        if (undo & WIFI_UNWIND_CLEAR) {
            wifi_eap_drv_clear(); // first: Enterprise off, and only what was set (an untouched old C6 gets no RPCs)
            atomic_store(&enterprise_on, false);
        }
        if (undo & WIFI_UNWIND_RESTORE) {
            esp_wifi_set_config(WIFI_IF_STA, prev); // then the previous config, while config_bad is not set yet
        }
        failed(why); // last: from here nothing connects until another pair applies
    }
    if (prev) {
        memset(prev, 0, sizeof(*prev)); // a previous PSK config holds its password
        lock->release(prev);
    }
    return why == WIFI_EAP_FAIL_NONE && config_done(ESP_OK, eap);
}

bool wifi_apply_net(char const *s, char const *p, wifi_eap_t const *eap, wifi_lock_t const *lock) {
    if (eap) {
        return apply_eap(s, eap, lock);
    }
    if (wifi_apply_plan(false, false, false, atomic_load(&enterprise_on)) == WIFI_PLAN_PSK_CLEAR) {
        wifi_eap_drv_clear();
        atomic_store(&enterprise_on, false);
    }
    return config_done(set_sta_config(s, p, false), NULL);
}

bool wifi_apply_saved(char const *s, char const *p, wifi_lock_t const *lock) {
    if (!s[0]) {
        return wifi_apply_net(s, p, NULL, lock); // forget: the driver's RAM copy goes
    }
    scratch_t *sc = lock->alloc(sizeof(*sc));
    if (!sc) {
        failed(WIFI_EAP_FAIL_NOMEM);
        return false;
    }
    lock->take(UINT32_MAX);
    wifi_eap_rec_t kind = wifi_cred_eap_record(s, sc->rec, sizeof(sc->rec));
    lock->give();
    bool rec_ok = kind == WIFI_EAP_REC_OK && wifi_eap_decode(sc->rec, &sc->e);
    bool ok     = false;
    switch (wifi_apply_plan(kind != WIFI_EAP_REC_NONE, rec_ok, strcmp(p, WIFI_SAVED_EAP_MARK) == 0,
                            atomic_load(&enterprise_on))) {
        case WIFI_PLAN_EAP: ok = wifi_apply_net(s, p, &sc->e, lock); break;
        case WIFI_PLAN_FAIL: failed(WIFI_EAP_FAIL_CRED); break;
        default: ok = wifi_apply_net(s, p, NULL, lock); break; // PSK, clearing what a previous network left
    }
    scratch_free(sc, lock);
    return ok;
}

bool wifi_apply_save_trial(char const *ssid, wifi_eap_t const *eap, wifi_lock_t const *lock) {
    scratch_t *sc = lock->alloc(sizeof(*sc));
    if (!sc) {
        return false;
    }
    bool ok = wifi_eap_encode(eap, sc->rec, sizeof(sc->rec)) > 0 && wifi_cred_save_eap(ssid, sc->rec);
    scratch_free(sc, lock); // the encoded record holds the password
    return ok;
}

void wifi_apply_console_eap(char const *args, wifi_lock_t const *lock) {
    char        ssid[33], a[140];
    wifi_eap_t *e = lock->alloc(sizeof(*e)); // 350 B: not on the console task's stack
    if (!e) {
        console_reply("wifi: out of memory");
        return;
    }
    wifi_eap_parse_t r = wifi_eap_parse_line(args, ssid, e);
    if (r == WIFI_EAP_PARSE_USAGE) {
        console_reply("wifi eap <ssid> <peap|ttls> <mschapv2|pap> <identity|-> <username> <ca-path|-> <password> (peap takes mschapv2; - = none; the password is the rest of the line)");
    } else if (r == WIFI_EAP_PARSE_PAP_NO_CA) {
        console_reply("wifi: PAP needs a CA certificate (give a CA path, or use mschapv2)");
    } else if (r == WIFI_EAP_PARSE_INVALID) {
        console_reply("wifi: invalid field (SSID 1-32, identity 0-63, username 1-63, password 1-127 bytes, CA path /storage/<file>.pem or .crt)");
    } else {
        wifi_ssid_sanitize(ssid, a, sizeof(a));
        if (!wifi_set_eap_credentials(ssid, e)) {
            console_reply("wifi: could not start (no C6 link or busy)");
        } else {
            console_reply("wifi: trying \"%s\" (Enterprise%s), saved only if it gets an IP within %d s; fw wifi shows the result",
                          a, wifi_eap_unverified(e) ? ", server not verified: no CA" : "", WIFI_JOIN_TRIAL_MS / 1000);
        }
    }
    memset(e, 0, sizeof(*e));
    lock->release(e);
}

void wifi_apply_console_status(void) {
    bool ip = wifi_has_ip(NULL, 0);
    if (ip && wifi_eap_ca() == WIFI_EAP_CA_UNVERIFIED) {
        console_reply("wifi: WARNING unverified server: no CA set, an impostor access point could take the login; with no identity set the username also goes out in clear");
    } else if (ip && wifi_eap_ca() == WIFI_EAP_CA_VERIFIED) {
        console_reply("wifi: Enterprise, server certificate chain checked against the CA (no server name check)");
    }
    wifi_eap_fail_t why = wifi_eap_last_fail();
    if (why != WIFI_EAP_FAIL_NONE) {
        console_reply("wifi: Enterprise join not started: %s", wifi_eap_fail_text(why));
    }
}
