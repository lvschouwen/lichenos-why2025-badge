// Host test: a repeater's settings over its CLI (#117): rows, commands, replies, the filter and the sequence
#include "mesh_rset.h"

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

static size_t row(char const *key) {
    int i = rs_row_find(key);
    CHECK(i >= 0);
    return (size_t)(i < 0 ? 0 : i);
}

static void rows(void) {
    CHECK(rs_row_count() == RS_ROWS && rs_row_at(RS_ROWS) == NULL && rs_row_find("nope") == -1);
    CHECK(RS_ROWS <= 32); // want is a 32-bit mask
    CHECK(rs_row_readable(row("name")) && !rs_row_readable(row("password")) && !rs_row_readable(row("reboot")));
    CHECK(!rs_row_editable(row("role")) && !rs_row_editable(row("ver")) && rs_row_editable(row("reboot")));
    for (size_t i = 0; i < RS_ROWS; i++) {
        CHECK(strcmp(rs_row_at(i)->key, "guest.password") != 0 && strcmp(rs_row_at(i)->key, "prv.key") != 0);
    }
}

static void checks(void) {
    CHECK(rs_check(row("name"), "NL-HHW-TEST") == NULL);
    CHECK(rs_check(row("name"), "  spaced  ") == NULL);
    CHECK(rs_check(row("name"), "") != NULL);
    CHECK(rs_check(row("name"), "a,b") != NULL && rs_check(row("name"), "x[1]") != NULL);
    CHECK(rs_check(row("name"), "12345678901234567890123456789012") != NULL); // 32
    CHECK(rs_check(row("tx"), "22") == NULL && rs_check(row("tx"), "-9") == NULL);
    CHECK(rs_check(row("tx"), "31") != NULL && rs_check(row("tx"), "2.5") != NULL && rs_check(row("tx"), "x") != NULL);
    CHECK(rs_check(row("advert.interval"), "0") == NULL && rs_check(row("advert.interval"), "120") == NULL);
    CHECK(rs_check(row("advert.interval"), "30") != NULL && rs_check(row("advert.interval"), "241") != NULL);
    CHECK(rs_check(row("flood.advert.interval"), "2") != NULL && rs_check(row("flood.advert.interval"), "12") == NULL);
    CHECK(rs_check(row("flood.max"), "64") == NULL && rs_check(row("flood.max"), "65") != NULL);
    CHECK(rs_check(row("af"), "1.5") == NULL && rs_check(row("af"), "10") != NULL);
    CHECK(rs_check(row("lat"), "52.07") == NULL && rs_check(row("lat"), "91") != NULL);
    CHECK(rs_check(row("lon"), "-4.3") == NULL && rs_check(row("lon"), "nan") != NULL);
    CHECK(rs_check(row("repeat"), "on") == NULL && rs_check(row("repeat"), "yes") != NULL);
    CHECK(rs_check(row("loop.detect"), "moderate") == NULL && rs_check(row("loop.detect"), "mod") != NULL);
    CHECK(rs_check(row("loop.detect"), "") != NULL);
    CHECK(rs_check(row("radio"), "869.618,62.5,7,5") == NULL && rs_check(row("radio"), "869.618 62.5 7 5") == NULL);
    CHECK(rs_check(row("radio"), "869.618,62.5,7") != NULL && rs_check(row("radio"), "869.618,62.5,13,5") != NULL);
    CHECK(rs_check(row("radio"), "100,62.5,7,5") != NULL && rs_check(row("radio"), "869,62.5,7,5,1") != NULL);
    CHECK(rs_check(row("radio"), "869,62.5,7.5,5") != NULL);
    CHECK(rs_check(row("role"), "x") != NULL);
    CHECK(rs_check(row("password"), "s3cret") == NULL && rs_check(row("password"), "") != NULL);
    CHECK(rs_check(row("password"), "1234567890123456") != NULL && rs_check(row("password"), " lead") != NULL);
    CHECK(rs_check(row("reboot"), "") == NULL);

    float   f, bw;
    uint8_t sf, cr;
    CHECK(rs_radio_parse("869.618,62.5,7,5", &f, &bw, &sf, &cr) && sf == 7 && cr == 5 && bw == 62.5f);
    CHECK(f > 869.61f && f < 869.62f);
}

static void commands(void) {
    char out[RS_CMD_MAX + 1];
    CHECK(rs_get_cmd(row("name"), "a7", out, sizeof(out)) && strcmp(out, "a7|get name") == 0);
    CHECK(rs_get_cmd(row("flood.max"), NULL, out, sizeof(out)) && strcmp(out, "get flood.max") == 0);
    CHECK(rs_get_cmd(row("ver"), "b1", out, sizeof(out)) && strcmp(out, "b1|ver") == 0);
    CHECK(!rs_get_cmd(row("password"), "a7", out, sizeof(out)) && !rs_get_cmd(row("reboot"), "a7", out, sizeof(out)));
    CHECK(rs_set_cmd(row("tx"), " 20 ", "c3", out, sizeof(out)) && strcmp(out, "c3|set tx 20") == 0);
    CHECK(rs_set_cmd(row("radio"), "869.618 62.5  7,5", "c4", out, sizeof(out)) &&
          strcmp(out, "c4|set radio 869.618,62.5,7,5") == 0);
    CHECK(rs_set_cmd(row("password"), "new pw", "c5", out, sizeof(out)) && strcmp(out, "c5|password new pw") == 0);
    CHECK(rs_set_cmd(row("reboot"), "", "c6", out, sizeof(out)) && strcmp(out, "c6|reboot") == 0);
    CHECK(!rs_set_cmd(row("tx"), "99", "c7", out, sizeof(out)));
    CHECK(!rs_set_cmd(row("role"), "x", "c7", out, sizeof(out)));
    char small[8];
    CHECK(!rs_set_cmd(row("password"), "abcdef", "c8", small, sizeof(small)) && small[0] == 0);
    // every row's commands are what the repeater's command class says they are
    CHECK(rs_set_cmd(row("radio"), "869.618,62.5,7,5", "c4", out, sizeof(out)) && rs_cmd_class(out) == RS_CMD_RADIO);
    CHECK(rs_set_cmd(row("password"), "x", "c4", out, sizeof(out)) && rs_cmd_class(out) == RS_CMD_SECRET);
    CHECK(rs_set_cmd(row("reboot"), "", "c4", out, sizeof(out)) && rs_cmd_class(out) == RS_CMD_REBOOT);
    for (size_t i = 0; i < RS_ROWS; i++) {
        if (rs_get_cmd(i, "zz", out, sizeof(out))) {
            CHECK(rs_cmd_class(out) == RS_CMD_PLAIN);
        }
    }
}

static void replies(void) {
    char out[RS_VALUE_MAX];
    size_t name = row("name"), tx = row("tx");
    CHECK(rs_parse_reply(name, false, "a7", "a7|> NL-HHW-TEST", out, sizeof(out)) == RS_R_VALUE &&
          strcmp(out, "NL-HHW-TEST") == 0);
    CHECK(rs_parse_reply(name, false, "a7", "a6|> old\na7|> new", out, sizeof(out)) == RS_R_VALUE &&
          strcmp(out, "new") == 0);
    CHECK(rs_parse_reply(name, false, "a7", "a6|> late reply", out, sizeof(out)) == RS_R_NOT_OURS);
    CHECK(rs_parse_reply(name, false, "a7", "> untagged", out, sizeof(out)) == RS_R_NOT_OURS);
    CHECK(rs_parse_reply(name, false, NULL, "> untagged", out, sizeof(out)) == RS_R_VALUE && strcmp(out, "untagged") == 0);
    CHECK(rs_parse_reply(tx, false, "a7", "a7|??: tx", out, sizeof(out)) == RS_R_UNSUPPORTED);
    CHECK(rs_parse_reply(tx, false, "a7", "a7|unknown config: ", out, sizeof(out)) == RS_R_UNSUPPORTED);
    CHECK(rs_parse_reply(tx, false, "a7", "a7|>", out, sizeof(out)) == RS_R_VALUE && out[0] == 0);
    CHECK(rs_parse_reply(row("ver"), false, "a7", "a7|v1.9.1 (Build: 1 Oct 2025)", out, sizeof(out)) == RS_R_VALUE &&
          strcmp(out, "v1.9.1 (Build: 1 Oct 2025)") == 0);
    CHECK(rs_parse_reply(row("ver"), false, "a7", "a7|Unknown command", out, sizeof(out)) == RS_R_UNSUPPORTED);
    CHECK(rs_parse_reply(tx, true, "a7", "a7|OK", out, sizeof(out)) == RS_R_OK);
    CHECK(rs_parse_reply(row("radio"), true, "a7", "a7|OK - reboot to apply", out, sizeof(out)) == RS_R_OK &&
          strcmp(out, "OK - reboot to apply") == 0);
    CHECK(rs_parse_reply(row("flood.max"), true, "a7", "a7|Error, max 64", out, sizeof(out)) == RS_R_ERROR &&
          strcmp(out, "Error, max 64") == 0);
    CHECK(rs_parse_reply(row("password"), true, "a7", "a7|password now: s3cret", out, sizeof(out)) == RS_R_OK &&
          !strstr(out, "s3cret"));
    char tiny[4];
    CHECK(rs_parse_reply(name, false, "a7", "a7|> a long name", tiny, sizeof(tiny)) == RS_R_VALUE &&
          strcmp(tiny, "a l") == 0);
}

static void filter(void) {
    CHECK(rs_cmd_class("erase") == RS_CMD_FORBIDDEN);
    CHECK(rs_cmd_class("  erase") == RS_CMD_FORBIDDEN);
    CHECK(rs_cmd_class("a7|erase") == RS_CMD_FORBIDDEN);
    CHECK(rs_cmd_class("erase now") == RS_CMD_FORBIDDEN);
    CHECK(rs_cmd_class("log erase") == RS_CMD_PLAIN && rs_cmd_class("eraser") == RS_CMD_PLAIN);
    CHECK(rs_cmd_class("set radio 869,62.5,7,5") == RS_CMD_RADIO && rs_cmd_class("set freq 869") == RS_CMD_RADIO);
    CHECK(rs_cmd_class("tempradio 869,62.5,7,5,10") == RS_CMD_RADIO);
    CHECK(rs_cmd_class("set radio.rxgain on") == RS_CMD_PLAIN && rs_cmd_class("get radio") == RS_CMD_PLAIN);
    CHECK(rs_cmd_class("password x") == RS_CMD_SECRET && rs_cmd_class("set guest.password x") == RS_CMD_SECRET);
    CHECK(rs_cmd_class("set prv.key ab") == RS_CMD_SECRET);
    CHECK(rs_cmd_class("reboot") == RS_CMD_REBOOT && rs_cmd_class("b2|clkreboot") == RS_CMD_REBOOT);
    CHECK(rs_cmd_class("start ota") == RS_CMD_REBOOT && rs_cmd_class("get name") == RS_CMD_PLAIN);

    char out[64];
    rs_redact_cmd("password s3cret", out, sizeof(out));
    CHECK(strcmp(out, "password (hidden)") == 0);
    rs_redact_cmd("a7|set guest.password abc", out, sizeof(out));
    CHECK(strcmp(out, "a7|set guest.password (hidden)") == 0);
    rs_redact_cmd("get name", out, sizeof(out));
    CHECK(strcmp(out, "get name") == 0);
    rs_redact_reply("a7|password now: s3cret", out, sizeof(out));
    CHECK(strcmp(out, "a7|password now: (hidden)") == 0);
    rs_redact_reply("Unknown command", out, sizeof(out));
    CHECK(strcmp(out, "Unknown command") == 0);
}

// What the CLI would answer, tagged like the command
static char const *answer(char const *cmd, char const *body, char *buf, size_t cap) {
    snprintf(buf, cap, "%.2s|%s", cmd, body);
    return buf;
}

static void sequence(void) {
    rs_t s;
    char cmd[RS_CMD_MAX + 1], buf[128], text[RS_VALUE_MAX];
    rs_init(&s);
    CHECK(!rs_busy(&s) && !rs_next(&s, cmd, sizeof(cmd)));
    rs_read_all(&s);
    CHECK(rs_busy(&s));
    size_t done, total;
    rs_progress(&s, &done, &total);
    CHECK(done == 0 && total == RS_ROWS - 2);
    rs_value_text(&s, row("name"), text, sizeof(text));
    CHECK(strcmp(text, "...") == 0);

    // one at a time, in row order
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get name"));
    CHECK(!rs_next(&s, cmd + 0, sizeof(cmd))); // in flight
    char first[RS_CMD_MAX + 1];
    snprintf(first, sizeof(first), "%s", cmd);
    CHECK(!rs_on_answer(&s, "zz|> someone else's")); // not ours: still waiting
    CHECK(rs_on_answer(&s, answer(first, "> NL-HHW-TEST", buf, sizeof(buf))));
    rs_value_text(&s, row("name"), text, sizeof(text));
    CHECK(strcmp(text, "NL-HHW-TEST") == 0);

    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get radio") && strncmp(cmd, first, 2) != 0);
    CHECK(rs_on_answer(&s, answer(cmd, "> 869.618,62.5,7,5", buf, sizeof(buf))));
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get tx"));
    CHECK(rs_on_answer(&s, answer(cmd, "> 22", buf, sizeof(buf))));
    rs_value_text(&s, row("tx"), text, sizeof(text));
    CHECK(strcmp(text, "22 dBm") == 0);
    rs_edit_text(&s, row("tx"), text, sizeof(text));
    CHECK(strcmp(text, "22") == 0);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get repeat"));
    CHECK(rs_on_answer(&s, NULL)); // no answer
    rs_value_text(&s, row("repeat"), text, sizeof(text));
    CHECK(strcmp(text, "\xE2\x80\x94") == 0);
    rs_edit_text(&s, row("repeat"), text, sizeof(text));
    CHECK(text[0] == 0);

    // a set goes before the remaining gets, then its row is read back
    CHECK(rs_queue_set(&s, row("tx"), "99") != NULL); // refused value: nothing queued
    CHECK(rs_queue_set(&s, row("tx"), "20") == NULL);
    CHECK(rs_queue_set(&s, row("flood.max"), "3") != NULL); // one change at a time
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|set tx 20"));
    CHECK(rs_queue_set(&s, row("flood.max"), "3") != NULL); // still waiting for its answer
    CHECK(rs_on_answer(&s, answer(cmd, "OK", buf, sizeof(buf))));
    CHECK(s.last == RS_R_OK && s.last_row == (int)row("tx"));
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get tx"));
    CHECK(rs_on_answer(&s, answer(cmd, "> 20", buf, sizeof(buf))));
    rs_value_text(&s, row("tx"), text, sizeof(text));
    CHECK(strcmp(text, "20 dBm") == 0);

    // a refused set is reported and not read back
    CHECK(rs_queue_set(&s, row("flood.max"), "3") == NULL);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|set flood.max 3"));
    CHECK(rs_on_answer(&s, answer(cmd, "Error, max 64", buf, sizeof(buf))));
    CHECK(s.last == RS_R_ERROR && strcmp(s.last_text, "Error, max 64") == 0);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get advert.interval"));
    CHECK(rs_on_answer(&s, answer(cmd, "??: advert.interval", buf, sizeof(buf))));
    rs_value_text(&s, row("advert.interval"), text, sizeof(text));
    CHECK(strcmp(text, "\xE2\x80\x94") == 0);

    // the password is wiped from the queue once its command is made; its echo is not kept
    CHECK(rs_queue_set(&s, row("password"), "n3wpw") == NULL && strcmp(s.set_value, "n3wpw") == 0);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|password n3wpw"));
    CHECK(s.set_value[0] == 0);
    CHECK(rs_on_answer(&s, answer(cmd, "password now: n3wpw", buf, sizeof(buf))));
    CHECK(s.last == RS_R_OK && !strstr(s.last_text, "n3wpw"));
    // a reboot never answers: that is fine
    CHECK(rs_queue_set(&s, row("reboot"), "") == NULL);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|reboot"));
    CHECK(rs_on_answer(&s, NULL) && s.last == RS_R_OK);
    // a set that could not be sent says so
    CHECK(rs_queue_set(&s, row("repeat"), "off") == NULL);
    CHECK(rs_next(&s, cmd, sizeof(cmd)));
    rs_on_unsent(&s, "MeshCore is not running");
    CHECK(s.last == RS_R_ERROR && strstr(s.last_text, "not running"));

    // the rest drains, and the whole read ends
    int guard = 0;
    while (rs_next(&s, cmd, sizeof(cmd)) && guard++ < 40) {
        rs_on_answer(&s, answer(cmd, "> x", buf, sizeof(buf)));
    }
    CHECK(!rs_busy(&s));
    rs_progress(&s, &done, &total);
    CHECK(done == total);
}

static void untagged_repeater(void) {
    rs_t s;
    char cmd[RS_CMD_MAX + 1];
    rs_init(&s);
    rs_read_all(&s);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && cmd[2] == '|');
    CHECK(rs_on_answer(&s, "Unknown command")); // an older repeater: the tag is not understood
    CHECK(s.untagged && s.st[row("name")] == RS_V_UNKNOWN);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strcmp(cmd, "get name") == 0);
    CHECK(rs_on_answer(&s, "> old-repeater"));
    CHECK(s.st[row("name")] == RS_V_VALUE && strcmp(s.value[row("name")], "old-repeater") == 0);

    // only the first command probes: later, an untagged line is a late or stray reply, not ours
    rs_t t;
    char buf[64];
    rs_init(&t);
    rs_read_all(&t);
    CHECK(rs_next(&t, cmd, sizeof(cmd)));
    CHECK(rs_on_answer(&t, answer(cmd, "> a", buf, sizeof(buf))));
    CHECK(rs_next(&t, cmd, sizeof(cmd)));
    CHECK(!rs_on_answer(&t, "Unknown command") && !t.untagged);
}

int main(void) {
    rows();
    checks();
    commands();
    replies();
    filter();
    sequence();
    untagged_repeater();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_rset_test: all checks passed\n");
    return 0;
}
