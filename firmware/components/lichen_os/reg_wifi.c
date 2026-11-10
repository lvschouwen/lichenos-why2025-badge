#include "reg_wifi.h"

#include <string.h>

bool reg_wifi_is_key(reg_entry_t const *e) {
    return e && REG_ID_DOMAIN(e->id) == REG_ID_DOMAIN(REG_ID_WIFI_COUNTRY);
}

static bool applied_of(uint16_t id, wifi_readback_t const *r, reg_value_t *out) {
    switch (id) {
        case REG_ID_WIFI_COUNTRY: return r->country_ok && wifi_country_text(r->cc, out->s, sizeof(out->s)) > 0;
        case REG_ID_WIFI_PROTOCOL: out->u = r->proto; return r->proto_ok;
        case REG_ID_WIFI_TXPWR: out->i = r->tx_raw; return r->tx_ok;
        case REG_ID_WIFI_BW: out->u = r->bw_mhz; return r->bw_ok && r->bw_mhz;
        case REG_ID_WIFI_PS: out->u = r->ps; return r->ps_ok;
        default: return false;
    }
}

static bool requested_of(uint16_t id, wifi_readback_t const *r, reg_value_t *out) {
    switch (id) {
        case REG_ID_WIFI_COUNTRY: memcpy(out->s, WIFI_COUNTRY, sizeof(WIFI_COUNTRY)); return true;
        case REG_ID_WIFI_PROTOCOL: out->u = r->req_proto; return r->req_proto != 0;
        case REG_ID_WIFI_TXPWR: out->i = r->req_tx_raw; return r->req_tx_raw != 0;
        case REG_ID_WIFI_BW: out->u = r->req_bw_mhz; return r->req_bw_mhz != 0;
        case REG_ID_WIFI_PS: out->u = r->req_ps; return true;
        default: return false;
    }
}

void reg_wifi_view(reg_entry_t const *e, wifi_readback_t const *r, bool fresh, bool live, uint32_t now_ms,
                   reg_view_t *v) {
    memset(v, 0, sizeof(*v));
    v->has_requested = requested_of(e->id, r, &v->requested);
    if (!r->have) {
        v->note = RV_NO_LINK;
        return;
    }
    v->has_age     = true;
    v->age_ms      = now_ms - r->at_ms;
    v->has_applied = applied_of(e->id, r, &v->applied) && reg_check(e, &v->applied) == REG_OK;
    if (!v->has_applied) {
        memset(&v->applied, 0, sizeof(v->applied));
        v->note = RV_FAILED;
    } else if (live && !fresh) {
        v->note = RV_STALE;
    }
}
