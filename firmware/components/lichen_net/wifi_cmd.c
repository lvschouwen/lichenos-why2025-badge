#include "wifi_cmd.h"

#include <stdlib.h>

#include <string.h>

#define TX_RAW_MIN 8
#define TX_RAW_MAX 80

static bool is(char const *args, char const *word) {
    return strcmp(args, word) == 0;
}

// `word` alone, or `word ` followed by anything
static bool is_or_has_arg(char const *args, char const *word) {
    size_t n = strlen(word);
    return strncmp(args, word, n) == 0 && (args[n] == '\0' || args[n] == ' ');
}

static void parse_ps(char const *args, wifi_cmd_t *out) {
    if (!args[2]) {
        return;
    }
    out->set    = true;
    out->ps_min = args[3] == 'm';
    if (!is(args + 3, "none") && !is(args + 3, "min")) {
        out->kind = WIFI_CMD_PS_USAGE;
    }
}

static void parse_tx(char const *args, wifi_cmd_t *out) {
    out->tx_raw = -1; // bare "tx" reads back only
    if (!args[2]) {
        return;
    }
    long raw    = strtol(args + 3, NULL, 10); // like atoi: "abc" is 0 (the default), "12ab" is 12
    out->tx_raw = raw > TX_RAW_MAX + 1 ? TX_RAW_MAX + 1 : raw < -2 ? -2 : (int)raw; // only the range matters
    if (out->tx_raw != -1 && out->tx_raw != 0 && (out->tx_raw < TX_RAW_MIN || out->tx_raw > TX_RAW_MAX)) {
        out->kind = WIFI_CMD_TX_USAGE;
    }
}

static void parse_join(char const *args, wifi_cmd_t *out) {
    // The SSID is the first word; the password is the rest of the line (it may contain spaces)
    char const *space = strchr(args, ' ');
    size_t      len   = space ? (size_t)(space - args) : strlen(args);
    out->pass         = space ? space + 1 : "";
    if (len == 0 || len > WIFI_CMD_SSID_MAX) {
        out->kind = WIFI_CMD_JOIN_USAGE;
        return;
    }
    out->kind = WIFI_CMD_JOIN;
    memcpy(out->ssid, args, len);
    out->ssid[len] = '\0';
}

static void fill_switch(char const *args, wifi_cmd_t *out) {
    out->on = args[1] == 'n';
}

static void fill_proto(char const *args, wifi_cmd_t *out) {
    out->kind    = args[5] ? WIFI_CMD_PROTO : WIFI_CMD_PROTO_USAGE;
    out->proto_n = args[5] && args[6] == 'n';
}

static void fill_bw(char const *args, wifi_cmd_t *out) {
    out->bw_mhz = !args[2] ? 0 : args[3] == '2' ? 20 : 40;
}

typedef struct {
    char const     *word;
    bool            with_arg; // also `word <argument>`; else the whole line must equal `word`
    wifi_cmd_kind_t kind;
    void (*fill)(char const *args, wifi_cmd_t *out); // may change kind to a usage kind
} rule_t;

// First match wins; a line matching no rule is a join
static rule_t const rules[] = {
    {"on", false, WIFI_CMD_SWITCH, fill_switch},
    {"off", false, WIFI_CMD_SWITCH, fill_switch},
    {"ps", true, WIFI_CMD_PS, parse_ps},
    {"stats", false, WIFI_CMD_STATS, NULL},
    {"tx", true, WIFI_CMD_TX, parse_tx},
    {"proto", false, WIFI_CMD_PROTO, fill_proto},
    {"proto n", false, WIFI_CMD_PROTO, fill_proto},
    {"proto ax", false, WIFI_CMD_PROTO, fill_proto},
    {"bw", false, WIFI_CMD_BW, fill_bw},
    {"bw 20", false, WIFI_CMD_BW, fill_bw},
    {"bw 40", false, WIFI_CMD_BW, fill_bw},
    {"--clear", false, WIFI_CMD_CLEAR, NULL},
    {"reconnect", false, WIFI_CMD_RECONNECT, NULL},
    {"scan", false, WIFI_CMD_SCAN, NULL},
};

void wifi_cmd_parse(char const *args, wifi_cmd_t *out) {
    memset(out, 0, sizeof(*out));
    out->pass = "";
    if (!*args) {
        out->kind = WIFI_CMD_STATUS;
        return;
    }
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        rule_t const *r = &rules[i];
        if (r->with_arg ? is_or_has_arg(args, r->word) : is(args, r->word)) {
            out->kind = r->kind;
            if (r->fill) {
                r->fill(args, out);
            }
            return;
        }
    }
    parse_join(args, out);
}
