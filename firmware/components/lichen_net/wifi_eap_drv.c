#include "wifi_eap_drv.h"

#include "esp_eap_client.h"
#include "esp_wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static wifi_eap_fail_t step(esp_err_t err, wifi_eap_step_t which) {
    return wifi_eap_rpc_fail((int)err, which);
}

// Reads the whole CA file into a block from mem. The caller sends len + 1 bytes (the NUL) and releases the block.
static wifi_eap_fail_t ca_read(char const *path, wifi_lock_t const *mem, char **out, size_t *len) {
    *out = NULL;
    char *buf = mem->alloc(WIFI_EAP_CA_FILE_MAX + 2);
    if (!buf) {
        return WIFI_EAP_FAIL_NOMEM;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        mem->release(buf);
        return WIFI_EAP_FAIL_CA_FILE;
    }
    // One byte more than the bound: a larger file shows up as too large instead of being sent cut
    size_t n = fread(buf, 1, WIFI_EAP_CA_FILE_MAX + 1, f);
    bool   bad = ferror(f) != 0;
    fclose(f);
    if (bad || !wifi_eap_pem_ok(buf, n)) {
        mem->release(buf);
        return WIFI_EAP_FAIL_CA_FILE;
    }
    buf[n] = '\0';
    *out   = buf;
    *len   = n;
    return WIFI_EAP_FAIL_NONE;
}

// Sends the CA read earlier (pem NULL: this network has none, so the previous one's is cleared)
static wifi_eap_fail_t ca_send(char const *pem, size_t len) {
    if (!pem) {
        esp_eap_client_clear_ca_cert(); // the previous network's CA must not make this one "verified"
        return WIFI_EAP_FAIL_NONE;
    }
    return step(esp_eap_client_set_ca_cert((unsigned char const *)pem, (int)len + 1), WIFI_EAP_STEP_CA);
}

static wifi_eap_fail_t method_set(wifi_eap_t const *e) {
    bool ttls = e->method == WIFI_EAP_TTLS;
    wifi_eap_fail_t why = step(esp_eap_client_set_eap_methods(ttls ? ESP_EAP_TYPE_TTLS : ESP_EAP_TYPE_PEAP),
                               WIFI_EAP_STEP_OTHER);
    if (why == WIFI_EAP_FAIL_NONE && ttls) {
        why = step(esp_eap_client_set_ttls_phase2_method(e->phase2 == WIFI_EAP_P2_PAP ? ESP_EAP_TTLS_PHASE2_PAP
                                                                                     : ESP_EAP_TTLS_PHASE2_MSCHAPV2),
                   WIFI_EAP_STEP_OTHER);
    }
    return why;
}

// One RPC; touched records that the C6 holds Enterprise state from here on (an old C6 fails the first one: untouched)
static wifi_eap_fail_t rpc(esp_err_t err, wifi_eap_step_t which, bool *touched) {
    wifi_eap_fail_t why = step(err, which);
    *touched = *touched || why == WIFI_EAP_FAIL_NONE;
    return why;
}

// A server name is on the C6 (an earlier join set one): see wifi_eap_name_plan. A C6 reset loses it (wifi_eap_drv_reset).
static atomic_bool name_on_c6;

// First thing of a join whose credential has no server name while an earlier one left a name on the C6: Enterprise is
// switched off, the only call that drops the name (RPC 382 cannot send an empty one), before anything is set again.
static wifi_eap_fail_t name_drop(wifi_eap_t const *e, bool *touched) {
    if (wifi_eap_name_plan(e->server[0] != '\0', atomic_load(&name_on_c6)) != WIFI_NAME_DISABLE_FIRST) {
        return WIFI_EAP_FAIL_NONE;
    }
    wifi_eap_fail_t why = rpc(esp_wifi_sta_enterprise_disable(), WIFI_EAP_STEP_OTHER, touched);
    if (why == WIFI_EAP_FAIL_NONE) {
        atomic_store(&name_on_c6, false); // only once it is gone: a failed disable leaves the name to be dropped next time
    }
    return why;
}

// The server name, after the CA: a name replaces the old one, no name sends nothing (name_drop saw to a stale one)
static wifi_eap_fail_t name_set(wifi_eap_t const *e, bool *touched) {
    if (wifi_eap_name_plan(e->server[0] != '\0', atomic_load(&name_on_c6)) != WIFI_NAME_SET) {
        return WIFI_EAP_FAIL_NONE;
    }
    wifi_eap_fail_t why = rpc(esp_eap_client_set_domain_name(e->server), WIFI_EAP_STEP_OTHER, touched);
    if (why == WIFI_EAP_FAIL_NONE) {
        atomic_store(&name_on_c6, true);
    }
    return why;
}

static wifi_eap_fail_t creds_send(wifi_eap_t const *e, bool *touched) {
    // An empty outer identity sends the username (a real one is usually "anonymous@realm"): in clear, to any AP
    char const *ident = e->identity[0] ? e->identity : e->username;
    wifi_eap_fail_t why = rpc(esp_eap_client_set_identity((unsigned char const *)ident, (int)strlen(ident)),
                              WIFI_EAP_STEP_OTHER, touched);
    if (why == WIFI_EAP_FAIL_NONE) {
        why = rpc(esp_eap_client_set_username((unsigned char const *)e->username, (int)strlen(e->username)),
                  WIFI_EAP_STEP_OTHER, touched);
    }
    if (why == WIFI_EAP_FAIL_NONE) {
        why = rpc(esp_eap_client_set_password((unsigned char const *)e->password, (int)strlen(e->password)),
                  WIFI_EAP_STEP_OTHER, touched);
    }
    return why;
}

wifi_eap_fail_t wifi_eap_drv_set(wifi_eap_t const *e, wifi_lock_t const *mem, bool *touched) {
    *touched = false;
    if (!wifi_eap_valid(e) || !mem || !mem->alloc) {
        return WIFI_EAP_FAIL_CRED;
    }
    // The CA file is read first: the file system frames never stack on a caller that holds a wifi_config_t
    char  *pem = NULL;
    size_t len = 0;
    wifi_eap_fail_t why = e->ca_path[0] ? ca_read(e->ca_path, mem, &pem, &len) : WIFI_EAP_FAIL_NONE;
    if (why == WIFI_EAP_FAIL_NONE) {
        why = name_drop(e, touched);
    }
    if (why == WIFI_EAP_FAIL_NONE) {
        why = creds_send(e, touched);
    }
    if (why == WIFI_EAP_FAIL_NONE) {
        why = method_set(e);
    }
    if (why == WIFI_EAP_FAIL_NONE) {
        why = ca_send(pem, len);
    }
    if (why == WIFI_EAP_FAIL_NONE) {
        why = name_set(e, touched);
    }
    if (pem) {
        mem->release(pem);
    }
    return why;
}

wifi_eap_fail_t wifi_eap_drv_enable(void) {
    return step(esp_wifi_sta_enterprise_enable(), WIFI_EAP_STEP_OTHER);
}

void wifi_eap_drv_reset(void) {
    atomic_store(&name_on_c6, false);
}

void wifi_eap_drv_clear(void) {
    esp_wifi_sta_enterprise_disable(); // also drops the server name (esp_eap_client.c: eap_globals_reset)
    atomic_store(&name_on_c6, false);
    esp_eap_client_clear_password();
    esp_eap_client_clear_username();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_ca_cert();
}
