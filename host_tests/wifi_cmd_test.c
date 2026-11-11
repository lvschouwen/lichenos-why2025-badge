// Host test: `fw wifi ...` argument classification (#207). The expectations were written from the old cmd_wifi
// if-chain, quirks included (`wifi on hunter2` joins "on"; bare `tx 0`, `tx abc` reset the power; `tx -1` only reads
// back).
#include "wifi_cmd.h"

#include <stdio.h>

#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static wifi_cmd_t parse(char const *args) {
    wifi_cmd_t c;
    wifi_cmd_parse(args, &c);
    return c;
}

static void whole_strings(void) {
    CHECK(parse("").kind == WIFI_CMD_STATUS);
    CHECK(parse("on").kind == WIFI_CMD_SWITCH && parse("on").on);
    CHECK(parse("off").kind == WIFI_CMD_SWITCH && !parse("off").on);
    CHECK(parse("stats").kind == WIFI_CMD_STATS);
    CHECK(parse("scan").kind == WIFI_CMD_SCAN);
    CHECK(parse("reconnect").kind == WIFI_CMD_RECONNECT);
    CHECK(parse("--clear").kind == WIFI_CMD_CLEAR);
}

// Anything but the exact word is a join: a typo must never run a subcommand
static void whole_string_only(void) {
    char const *const joins[] = {
        "scan hunter22",
        "on 12345678",
        "off x",
        "stats x",
        "reconnect now",
        "--clear x",
        "psx",
        "txx",
        "proto x",
        "proto n x",
        "bw 80",
        "bw 2",
        "bwx",
        " on"
    };
    for (size_t i = 0; i < sizeof(joins) / sizeof(joins[0]); i++) {
        wifi_cmd_t c = parse(joins[i]);
        if (c.kind != WIFI_CMD_JOIN && c.kind != WIFI_CMD_JOIN_USAGE) {
            printf("FAIL \"%s\" is not a join (kind %d)\n", joins[i], c.kind);
            failures++;
        }
    }
    wifi_cmd_t c = parse("on 12345678");
    CHECK(c.kind == WIFI_CMD_JOIN && strcmp(c.ssid, "on") == 0 && strcmp(c.pass, "12345678") == 0);
    CHECK(parse(" on").kind == WIFI_CMD_JOIN_USAGE); // leading space = empty SSID
}

static void ps(void) {
    wifi_cmd_t c = parse("ps");
    CHECK(c.kind == WIFI_CMD_PS && !c.set);
    c = parse("ps none");
    CHECK(c.kind == WIFI_CMD_PS && c.set && !c.ps_min);
    c = parse("ps min");
    CHECK(c.kind == WIFI_CMD_PS && c.set && c.ps_min);
    CHECK(parse("ps ").kind == WIFI_CMD_PS_USAGE);
    CHECK(parse("ps max").kind == WIFI_CMD_PS_USAGE);
    CHECK(parse("ps none x").kind == WIFI_CMD_PS_USAGE);
}

static void tx(void) {
    wifi_cmd_t c = parse("tx");
    CHECK(c.kind == WIFI_CMD_TX && c.tx_raw == -1);
    c = parse("tx 0");
    CHECK(c.kind == WIFI_CMD_TX && c.tx_raw == 0);
    c = parse("tx 34");
    CHECK(c.kind == WIFI_CMD_TX && c.tx_raw == 34);
    CHECK(parse("tx 8").kind == WIFI_CMD_TX && parse("tx 80").kind == WIFI_CMD_TX && parse("tx 80").tx_raw == 80);
    CHECK(parse("tx 7").kind == WIFI_CMD_TX_USAGE && parse("tx 81").kind == WIFI_CMD_TX_USAGE);
    CHECK(parse("tx -5").kind == WIFI_CMD_TX_USAGE);
    CHECK(parse("tx 99999999999999999999").kind == WIFI_CMD_TX_USAGE);
    c = parse("tx -1");
    CHECK(c.kind == WIFI_CMD_TX && c.tx_raw == -1); // read back only, like the old code
    c = parse("tx abc");
    CHECK(c.kind == WIFI_CMD_TX && c.tx_raw == 0); // atoi semantics: reset to the default
    c = parse("tx ");
    CHECK(c.kind == WIFI_CMD_TX && c.tx_raw == 0);
    c = parse("tx 12ab");
    CHECK(c.kind == WIFI_CMD_TX && c.tx_raw == 12);
}

static void proto_bw(void) {
    CHECK(parse("proto").kind == WIFI_CMD_PROTO_USAGE);
    wifi_cmd_t c = parse("proto n");
    CHECK(c.kind == WIFI_CMD_PROTO && c.proto_n);
    c = parse("proto ax");
    CHECK(c.kind == WIFI_CMD_PROTO && !c.proto_n);
    c = parse("bw");
    CHECK(c.kind == WIFI_CMD_BW && c.bw_mhz == 0);
    CHECK(parse("bw 20").kind == WIFI_CMD_BW && parse("bw 20").bw_mhz == 20);
    CHECK(parse("bw 40").kind == WIFI_CMD_BW && parse("bw 40").bw_mhz == 40);
}

static void join(void) {
    wifi_cmd_t c = parse("home pass word with spaces");
    CHECK(c.kind == WIFI_CMD_JOIN && strcmp(c.ssid, "home") == 0 && strcmp(c.pass, "pass word with spaces") == 0);
    c = parse("open");
    CHECK(c.kind == WIFI_CMD_JOIN && strcmp(c.ssid, "open") == 0 && c.pass[0] == '\0');
    char thirty2[40] = "12345678901234567890123456789012 pw";
    c                = parse(thirty2);
    CHECK(c.kind == WIFI_CMD_JOIN && strlen(c.ssid) == 32);
    char thirty3[40] = "123456789012345678901234567890123 pw";
    CHECK(parse(thirty3).kind == WIFI_CMD_JOIN_USAGE);
    // The password is a pointer into the parsed line, not a copy
    char       line[] = "net secret";
    wifi_cmd_t d;
    wifi_cmd_parse(line, &d);
    CHECK(d.pass == line + 4);
}

int main(void) {
    whole_strings();
    whole_string_only();
    ps();
    tx();
    proto_bw();
    join();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("wifi_cmd_test: ok\n");
    return 0;
}
