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
    CHECK(rs_cmd_class("set bridge.secret ab") == RS_CMD_SECRET && rs_cmd_class("a3|set bridge.secret ab") == RS_CMD_SECRET);
    CHECK(rs_cmd_class("  set bridge.secret ab") == RS_CMD_SECRET && rs_cmd_class("set bridge.enabled on") == RS_CMD_PLAIN);
    CHECK(rs_cmd_class("reboot") == RS_CMD_REBOOT && rs_cmd_class("b2|clkreboot") == RS_CMD_REBOOT);
    CHECK(rs_cmd_class("start ota") == RS_CMD_REBOOT && rs_cmd_class("get name") == RS_CMD_PLAIN);

    // #123: the console's confirm and the screen's dialog use one list
    CHECK(rs_cmd_needs_confirm("reboot") && rs_cmd_needs_confirm("set radio 869,62.5,7,5") && rs_cmd_needs_confirm("a7|reboot"));
    CHECK(!rs_cmd_needs_confirm("get name") && !rs_cmd_needs_confirm("password x") && !rs_cmd_needs_confirm("erase"));
    char cf[64];
    CHECK(rs_cmd_confirm("reboot", cf, sizeof(cf)) == RS_CONF_MISSING && strcmp(cf, "reboot") == 0);
    CHECK(rs_cmd_confirm("reboot YES", cf, sizeof(cf)) == RS_CONF_GIVEN && strcmp(cf, "reboot") == 0);
    CHECK(rs_cmd_confirm("reboot   YES  ", cf, sizeof(cf)) == RS_CONF_GIVEN && strcmp(cf, "reboot") == 0);
    CHECK(rs_cmd_confirm("set radio 869,62.5,7,5 YES", cf, sizeof(cf)) == RS_CONF_GIVEN && strcmp(cf, "set radio 869,62.5,7,5") == 0);
    CHECK(rs_cmd_confirm("reboot yes", cf, sizeof(cf)) == RS_CONF_MISSING && strcmp(cf, "reboot yes") == 0); // YES only
    CHECK(rs_cmd_confirm("reboot YESTERDAY", cf, sizeof(cf)) == RS_CONF_MISSING);
    CHECK(rs_cmd_confirm("get name", cf, sizeof(cf)) == RS_CONF_NOT_NEEDED && strcmp(cf, "get name") == 0);
    CHECK(rs_cmd_confirm("set name YES", cf, sizeof(cf)) == RS_CONF_NOT_NEEDED && strcmp(cf, "set name YES") == 0);
    CHECK(rs_cmd_confirm("erase YES", cf, sizeof(cf)) == RS_CONF_NOT_NEEDED && rs_cmd_class(cf) == RS_CMD_FORBIDDEN);
    CHECK(rs_cmd_confirm("YES", cf, sizeof(cf)) == RS_CONF_NOT_NEEDED);
    char tiny[8];
    CHECK(rs_cmd_confirm("set radio 869,62.5,7,5 YES", tiny, sizeof(tiny)) == RS_CONF_MISSING); // cut: never "given"

    char out[64];
    rs_redact_cmd("password s3cret", out, sizeof(out));
    CHECK(strcmp(out, "password (hidden)") == 0);
    rs_redact_cmd("a7|set guest.password abc", out, sizeof(out));
    CHECK(strcmp(out, "a7|set guest.password (hidden)") == 0);
    rs_redact_cmd("set bridge.secret hunter2", out, sizeof(out));
    CHECK(strcmp(out, "set bridge.secret (hidden)") == 0);
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
    CHECK(strcmp(text, "No answer") == 0);
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
    CHECK(strcmp(text, "No answer") == 0);

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

// Review fixes (#117): only plain decimal numbers go out as typed
static void plain_numbers(void) {
    char out[RS_CMD_MAX + 1];
    CHECK(rs_check(row("tx"), "3e1") != NULL && rs_check(row("tx"), "0x10") != NULL);
    CHECK(rs_check(row("af"), ".5e1") != NULL && rs_check(row("af"), ".5") != NULL && rs_check(row("af"), "1.") != NULL);
    CHECK(rs_check(row("af"), "+1") != NULL && rs_check(row("lat"), "inf") != NULL && rs_check(row("tx"), "-") != NULL);
    CHECK(rs_check(row("lat"), "-52.5") == NULL && rs_check(row("af"), "0.5") == NULL);
    CHECK(rs_check(row("radio"), "8.69e2,62.5,7,5") != NULL && rs_check(row("radio"), "869.618,0x3e,7,5") != NULL);
    CHECK(!rs_set_cmd(row("tx"), "2e1", "a1", out, sizeof(out)));
}

// A set queued before any get waits for a probing get; a stray untagged line later never turns the tags off
static void probe_first(void) {
    rs_t s;
    char cmd[RS_CMD_MAX + 1], buf[64];
    rs_init(&s);
    CHECK(rs_queue_set(&s, row("tx"), "20") == NULL);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get name") && s.tag_probe);
    CHECK(rs_on_answer(&s, answer(cmd, "> n", buf, sizeof(buf))));
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|set tx 20") && !s.tag_probe);
    CHECK(!rs_on_answer(&s, "Unknown command") && !s.untagged);

    rs_t t;
    rs_init(&t);
    rs_read_all(&t);
    CHECK(rs_next(&t, cmd, sizeof(cmd)) && t.tag_probe);
    CHECK(!rs_on_answer(&t, "> a stray untagged reply") && !t.untagged); // not a refusal of the tag
}

static void stops_and_retries(void) {
    rs_t s;
    char cmd[RS_CMD_MAX + 1], buf[64];
    rs_init(&s);
    rs_read_all(&s);
    // a get that could not be sent is tried again, RS_UNSENT_MAX times in all
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get name"));
    rs_on_unsent(&s, "busy");
    CHECK(s.st[row("name")] == RS_V_UNKNOWN && !s.probed);
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && strstr(cmd, "|get name") && s.tag_probe);
    rs_on_unsent(&s, "busy");
    CHECK(s.st[row("name")] == RS_V_NONE);
    // RS_MISSES_MAX gets in a row without an answer stop the read
    CHECK(rs_next(&s, cmd, sizeof(cmd)));
    CHECK(rs_on_answer(&s, answer(cmd, "> 869.618,62.5,7,5", buf, sizeof(buf))));
    for (int i = 0; i < RS_MISSES_MAX - 1; i++) {
        CHECK(rs_next(&s, cmd, sizeof(cmd)) && rs_on_answer(&s, NULL) && !s.stopped);
    }
    CHECK(rs_next(&s, cmd, sizeof(cmd)) && rs_on_answer(&s, NULL));
    CHECK(s.stopped && !rs_busy(&s) && !rs_next(&s, cmd, sizeof(cmd)));
    size_t done, total;
    rs_progress(&s, &done, &total);
    CHECK(done == total);
    rs_read_all(&s); // Read again starts over
    CHECK(!s.stopped && rs_busy(&s) && s.misses == 0);
}

// The repeater's raw floats show with a fixed precision, the radio as the badge shows its own (#131)
static void formatting(void) {
    char text[RS_VALUE_MAX];
    rs_format_value(row("radio"), "869.6179809,62.5,7,5", text, sizeof(text));
    CHECK(strcmp(text, "869.618 MHz, 62.5 kHz, SF7, CR5") == 0);
    rs_format_value(row("radio"), "910.525,250,10,8", text, sizeof(text));
    CHECK(strcmp(text, "910.525 MHz, 250 kHz, SF10, CR8") == 0);
    rs_format_value(row("radio"), "garbage", text, sizeof(text));
    CHECK(strcmp(text, "garbage") == 0);
    rs_format_value(row("lat"), "52.3702011", text, sizeof(text));
    CHECK(strcmp(text, "52.3702") == 0);
    rs_format_value(row("lon"), "-4.9", text, sizeof(text));
    CHECK(strcmp(text, "-4.9000") == 0);
    rs_format_value(row("af"), "1", text, sizeof(text));
    CHECK(strcmp(text, "1.00") == 0);
    rs_format_value(row("txdelay"), "0.5000001", text, sizeof(text));
    CHECK(strcmp(text, "0.50") == 0);
    rs_format_value(row("rxdelay"), "0", text, sizeof(text));
    CHECK(strcmp(text, "0.00") == 0);
    rs_format_value(row("repeat"), "on", text, sizeof(text));
    CHECK(strcmp(text, "On") == 0);
    rs_format_value(row("loop.detect"), "minimal", text, sizeof(text));
    CHECK(strcmp(text, "Minimal") == 0);
    rs_format_value(row("name"), "  Keep as is ", text, sizeof(text));
    CHECK(strcmp(text, "Keep as is") == 0);

    rs_t s;
    rs_init(&s);
    s.st[row("radio")] = RS_V_VALUE;
    snprintf(s.value[row("radio")], RS_VALUE_MAX, "869.6179809,62.5,7,5");
    rs_value_text(&s, row("radio"), text, sizeof(text));
    CHECK(strcmp(text, "869.618 MHz, 62.5 kHz, SF7, CR5") == 0);
    rs_edit_text(&s, row("radio"), text, sizeof(text));
    CHECK(strcmp(text, "869.618,62.5,7,5") == 0 && rs_check(row("radio"), text) == NULL);
    s.st[row("lat")] = RS_V_VALUE;
    snprintf(s.value[row("lat")], RS_VALUE_MAX, "52.3702011");
    rs_edit_text(&s, row("lat"), text, sizeof(text));
    CHECK(strcmp(text, "52.3702011") == 0); // editing keeps the full value
}

// The repeater's protocol words after a set become plain ones (R4, #137)
static void outcome(void) {
    char t[RS_TEXT_MAX];
    rs_outcome_words(row("tx"), RS_R_OK, "OK", t, sizeof(t));
    CHECK(strcmp(t, "Saved") == 0);
    rs_outcome_words(row("tx"), RS_R_OK, "OK - reboot to apply", t, sizeof(t));
    CHECK(strcmp(t, "Saved. It applies after a restart") == 0);
    rs_outcome_words(row("radio"), RS_R_OK, "OK", t, sizeof(t));
    CHECK(strstr(t, "changes over after a restart"));
    rs_outcome_words(row("reboot"), RS_R_OK, "x", t, sizeof(t));
    CHECK(strcmp(t, "Restart sent") == 0);
    rs_outcome_words(row("flood.max"), RS_R_ERROR, "Error, max 64", t, sizeof(t));
    CHECK(strcmp(t, "The repeater refused it: max 64") == 0);
    rs_outcome_words(row("flood.max"), RS_R_ERROR, "Not sent: busy", t, sizeof(t));
    CHECK(strcmp(t, "Not sent: busy") == 0);
}

static void note_pick(void) {
    // a one-off note with nothing sent yet must survive the tick (#140)
    CHECK(rs_note_pick(false, true, false, false) == RS_NOTE_LOCAL);
    // it is the newer event, so it shows over an older outcome or problem
    CHECK(rs_note_pick(false, true, true, true) == RS_NOTE_LOCAL);
    CHECK(rs_note_pick(true, true, true, true) == RS_NOTE_SENDING);
    CHECK(rs_note_pick(false, false, true, true) == RS_NOTE_OUTCOME);
    CHECK(rs_note_pick(false, false, false, true) == RS_NOTE_PROBLEM);
    CHECK(rs_note_pick(false, false, false, false) == RS_NOTE_NONE);
}

// Every accept and reject rs_check makes, with its exact message (#212: pinned before the validator was restructured)
typedef struct {
    char const *key, *value, *why; // why NULL: accepted
} check_case_t;

static check_case_t const CHECK_CASES[] = {
    {"reboot", "", NULL},
    {"reboot", "anything \x01", NULL},
    {"role", "x", "read only"},
    {"role", "", "read only"},
    {"ver", "1.0", "read only"},
    {"password", "s3cret", NULL},
    {"password", "123456789012345", NULL},
    {"password", "in side", NULL},
    {"password", "", "1 to 15 characters"},
    {"password", "1234567890123456", "1 to 15 characters"},
    {"password", " lead", "no spaces at the start or end"},
    {"password", "trail ", "no spaces at the start or end"},
    {"password", "ab\tcd", "no spaces at the start or end"},
    {"password", "ab\x7f", "no spaces at the start or end"},
    {"name", "NL-HHW-TEST", NULL},
    {"name", "  spaced  ", NULL},
    {"name", "1234567890123456789012345678901", NULL},
    {"name", "12345678901234567890123456789012", "1 to 31 characters"},
    {"name", "", "1 to 31 characters"},
    {"name", "   ", "1 to 31 characters"},
    {"name", "a\nb", "too long"},
    {"name", "a[", "none of [ ] \\ : , ? *"},
    {"name", "a]", "none of [ ] \\ : , ? *"},
    {"name", "a\\", "none of [ ] \\ : , ? *"},
    {"name", "a:", "none of [ ] \\ : , ? *"},
    {"name", "a,b", "none of [ ] \\ : , ? *"},
    {"name", "a?", "none of [ ] \\ : , ? *"},
    {"name", "a*", "none of [ ] \\ : , ? *"},
    {"tx", "22", NULL},
    {"tx", " -9 ", NULL},
    {"tx", "30", NULL},
    {"tx", "0", NULL},
    {"tx", "30.0", NULL},
    {"tx", "31", "-9 to 30"},
    {"tx", "-10", "-9 to 30"},
    {"tx", "2.5", "a whole number"},
    {"tx", "x", "a whole number"},
    {"tx", "", "a whole number"},
    {"tx", "+5", "a whole number"},
    {"tx", "3e1", "a whole number"},
    {"tx", "0x10", "a whole number"},
    {"tx", "1.", "a whole number"},
    {"tx", "-", "a whole number"},
    {"advert.interval", "0", NULL},
    {"advert.interval", "-0", NULL},
    {"advert.interval", "60", NULL},
    {"advert.interval", "240", NULL},
    {"advert.interval", "30", "0 (off) or 60 to 240"},
    {"advert.interval", "241", "0 (off) or 60 to 240"},
    {"flood.advert.interval", "0", NULL},
    {"flood.advert.interval", "2", "0 (off) or 3 to 168"},
    {"flood.advert.interval", "12", NULL},
    {"flood.max", "0", NULL},
    {"flood.max", "64", NULL},
    {"flood.max", "65", "0 to 64"},
    {"af", "1.5", NULL},
    {"af", "9", NULL},
    {"af", "10", "0 to 9"},
    {"af", "-0.1", "0 to 9"},
    {"af", "1e0", "a number"},
    {"af", ".5", "a number"},
    {"txdelay", "2", NULL},
    {"txdelay", "2.01", "0 to 2"},
    {"direct.txdelay", "0.3", NULL},
    {"rxdelay", "20.5", "0 to 20"},
    {"lat", "52.07", NULL},
    {"lat", "91", "-90 to 90"},
    {"lon", "-4.3", NULL},
    {"lon", "-180.5", "-180 to 180"},
    {"lon", "nan", "a number"},
    {"lon", "inf", "a number"},
    {"repeat", "on", NULL},
    {"repeat", " off ", NULL},
    {"repeat", "yes", "on or off"},
    {"repeat", "ON", "on or off"},
    {"allow.read.only", "off", NULL},
    {"allow.read.only", "", "on or off"},
    {"loop.detect", "off", NULL},
    {"loop.detect", "minimal", NULL},
    {"loop.detect", "moderate", NULL},
    {"loop.detect", "strict", NULL},
    {"loop.detect", "mod", "off, minimal, moderate or strict"},
    {"loop.detect", "off|minimal", "off, minimal, moderate or strict"},
    {"loop.detect", "", "off, minimal, moderate or strict"},
    {"radio", "869.618,62.5,7,5", NULL},
    {"radio", "869.618 62.5 7 5", NULL},
    {"radio", "150,7,5,5", NULL},
    {"radio", "2500,500,12,8", NULL},
    {"radio", "869.618,62.5,7", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
    {"radio", "869.618,62.5,13,5", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
    {"radio", "100,62.5,7,5", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
    {"radio", "869,62.5,7,5,1", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
    {"radio", "869,62.5,7.5,5", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
    {"radio", "869,6,7,5", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
    {"radio", "869,62.5,7,9", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
    {"radio", "", "MHz, kHz, SF, CR (150-2500, 7-500, 5-12, 5-8)"},
};

static void check_table(void) {
    for (size_t k = 0; k < sizeof(CHECK_CASES) / sizeof(CHECK_CASES[0]); k++) {
        check_case_t const *c   = &CHECK_CASES[k];
        char const         *why = rs_check(row(c->key), c->value);
        bool ok = c->why ? why != NULL && strcmp(why, c->why) == 0 : why == NULL;
        if (!ok) {
            printf("FAIL check %s=\"%s\": got \"%s\", want \"%s\"\n", c->key, c->value, why ? why : "(accepted)",
                   c->why ? c->why : "(accepted)");
            failures++;
        }
    }
    CHECK(strcmp(rs_check(RS_ROWS, "x"), "no such setting") == 0);
    CHECK(strcmp(rs_check((size_t)-1, "x"), "no such setting") == 0);
    // Past RS_CMD_MAX after the spaces come off: too long for every kind that is trimmed
    char long_value[RS_CMD_MAX + 2];
    memset(long_value, '1', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = 0;
    CHECK(strcmp(rs_check(row("tx"), long_value), "too long") == 0);
    CHECK(strcmp(rs_check(row("name"), long_value), "too long") == 0);
    long_value[RS_CMD_MAX] = 0; // exactly RS_CMD_MAX fits the buffer, then the kind's own rule speaks
    CHECK(strcmp(rs_check(row("name"), long_value), "1 to 31 characters") == 0);
    CHECK(strcmp(rs_check(row("tx"), long_value), "-9 to 30") == 0);
}

int main(void) {
    check_table();
    note_pick();
    outcome();
    formatting();
    rows();
    checks();
    commands();
    replies();
    filter();
    sequence();
    untagged_repeater();
    plain_numbers();
    probe_first();
    stops_and_retries();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_rset_test: all checks passed\n");
    return 0;
}
