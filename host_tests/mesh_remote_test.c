// Host test: log in, status and CLI state machine for a repeater or room (#51)
#include "mesh_remote.h"
#include "mesh_pin.h"
#include "mutate.h"

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

static uint8_t const A[6] = {1, 2, 3, 4, 5, 6}, B[6] = {9, 9, 9, 9, 9, 9};

static mf_msg_t sent_reply(uint32_t est) {
    mf_msg_t m = {.kind = MF_SENT, .flood = 1, .est_timeout = est};
    return m;
}

// A request takes the mesh task's turn, MyMesh's RESP_SENT, then an answer
static rr_op_t take(rr_t *r, uint32_t now) {
    rr_op_t op;
    uint8_t prefix[6];
    char    arg[RR_CMD_MAX + 1];
    CHECK(rr_take(r, &op, prefix, arg, sizeof(arg), now));
    return op;
}

static void login(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    CHECK(!rr_busy(&r) && !rr_logged_in(&r, A));
    CHECK(rr_request(&r, RR_LOGIN, A, "1234567890123456", 0, &why) == 0 && why); // 16 characters: refused
    why = NULL;
    uint32_t seq = rr_request(&r, RR_LOGIN, A, "secret", 100, &why);
    CHECK(seq == 1 && r.state == RR_QUEUED && rr_busy(&r));
    CHECK(rr_request(&r, RR_STATUS, A, NULL, 100, &why) == 0); // one at a time

    // the password is handed to the pipe once and gone from the request, also from every copy for a screen
    rr_t copy;
    rr_poll(&r, 100, &copy);
    CHECK(copy.arg[0] == 0 && strcmp(r.arg, "secret") == 0);
    rr_op_t op;
    uint8_t prefix[6];
    char    pw[RR_CMD_MAX + 1];
    CHECK(rr_take(&r, &op, prefix, pw, sizeof(pw), 110));
    CHECK(op == RR_LOGIN && strcmp(pw, "secret") == 0 && r.arg[0] == 0 && memcmp(prefix, A, 6) == 0);
    CHECK(!rr_take(&r, &op, prefix, pw, sizeof(pw), 110)); // taken once

    mf_msg_t sent = sent_reply(2000);
    rr_on_reply(&r, &sent, 120);
    CHECK(r.state == RR_SENT && r.flood == 1 && r.est_ms == 2000);

    mf_msg_t ok = {.kind = MF_LOGIN_SUCCESS, .admin = 1, .acl = 3, .fw_level = 8};
    memcpy(ok.rprefix, B, 6);
    CHECK(!rr_on_login(&r, &ok, 200)); // someone else's answer
    memcpy(ok.rprefix, A, 6);
    CHECK(rr_on_login(&r, &ok, 200) && r.state == RR_DONE && r.logged_in && r.admin && rr_logged_in(&r, A));
    CHECK(!rr_logged_in(&r, B));
    char text[96];
    rr_state_text(&r, text, sizeof(text));
    CHECK(strcmp(text, "Logged in as admin") == 0);

    // logging out is local
    rr_logout(&r, B);
    CHECK(rr_logged_in(&r, A));
    rr_logout(&r, A);
    CHECK(!rr_logged_in(&r, A) && !r.admin);
}

static void login_refused_and_timeout(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    rr_request(&r, RR_LOGIN, A, "bad", 0, &why);
    take(&r, 0);
    mf_msg_t sent = sent_reply(1000), fail = {.kind = MF_LOGIN_FAIL};
    rr_on_reply(&r, &sent, 10);
    memcpy(fail.rprefix, A, 6);
    CHECK(rr_on_login(&r, &fail, 50) && r.state == RR_FAILED && r.refused && !r.logged_in);
    char text[96];
    rr_state_text(&r, text, sizeof(text));
    CHECK(strstr(text, "wrong password") != NULL);

    // no answer: times out at est + grace, never earlier
    CHECK(rr_request(&r, RR_LOGIN, A, "x", 1000, &why) == 2);
    take(&r, 1000);
    rr_on_reply(&r, &sent, 1000);
    rr_poll(&r, 1000 + 5999, NULL);
    CHECK(r.state == RR_SENT);
    rr_poll(&r, 1000 + 8001, NULL);
    CHECK(r.state == RR_TIMEOUT && !rr_busy(&r));
    CHECK(rr_deadline_ms(0) == RR_FLOOR_MS && rr_deadline_ms(1000000) == RR_CAP_MS && rr_deadline_ms(20000) == 25000);

    // MyMesh refuses (no packet free), or the contact is gone
    CHECK(rr_request(&r, RR_STATUS, A, NULL, 2000, &why) == 3);
    take(&r, 2000);
    mf_msg_t err = {.kind = MF_ERR, .err = MF_ERR_TABLE_FULL};
    rr_on_reply(&r, &err, 2001);
    CHECK(r.state == RR_FAILED && r.err == MF_ERR_TABLE_FULL && strstr(r.why, "busy"));
    CHECK(rr_request(&r, RR_STATUS, A, NULL, 2000, &why) == 4);
    take(&r, 2000);
    rr_fail(&r, "contact not found");
    CHECK(r.state == RR_FAILED && strcmp(r.why, "contact not found") == 0);

    // a request the pipe never gets to (MeshCore stopped) ends
    CHECK(rr_request(&r, RR_STATUS, A, NULL, 3000, &why) == 5);
    rr_poll(&r, 3000 + RR_CAP_MS + 1, NULL);
    CHECK(r.state == RR_TIMEOUT);
}

// A queued login holds its password only until the pipe takes it, fails it or the request times out (review of #51)
static void password_wiped(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    rr_request(&r, RR_LOGIN, A, "secret", 0, &why);
    rr_fail(&r, "contact not found");
    CHECK(r.arg[0] == 0 && memcmp(r.arg, r.arg + 1, 6) == 0 && strstr(r.arg, "ecret") == NULL);
    rr_request(&r, RR_LOGIN, A, "secret", 0, &why);
    rr_poll(&r, RR_CAP_MS + 1, NULL);
    CHECK(r.state == RR_TIMEOUT && r.arg[0] == 0 && strstr(r.arg + 1, "ecret") == NULL);
}

static void status(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    rr_request(&r, RR_STATUS, A, "ignored", 0, &why);
    CHECK(take(&r, 0) == RR_STATUS);
    mf_msg_t sent = sent_reply(1000), st = {.kind = MF_STATUS};
    rr_on_reply(&r, &sent, 5);
    st.status.bytes = 56;
    st.status.batt_mv = 4123;
    st.status.up_secs = 3 * 86400 + 4 * 3600 + 5 * 60 + 9;
    st.status.tx_queue = 2;
    st.status.noise_floor = -111;
    st.status.last_snr_q = -6;
    memcpy(st.rprefix, B, 6);
    CHECK(!rr_on_status(&r, &st, 10));
    memcpy(st.rprefix, A, 6);
    CHECK(rr_on_status(&r, &st, 10) && r.state == RR_DONE && r.have_status);
    CHECK(!rr_on_status(&r, &st, 11)); // a second answer is not for a request that is done
    // the small copy carries what the status history needs (#168): target, age and the last status
    rr_head_t h;
    rr_poll_head(&r, 2500, &h);
    CHECK(h.op == RR_STATUS && h.state == RR_DONE && memcmp(h.prefix, A, 6) == 0 && h.age_ms == 2500 && !rr_head_busy(&h));
    CHECK(h.have_status && memcmp(h.status_prefix, A, 6) == 0 && h.status.batt_mv == 4123 &&
          h.status.noise_floor == -111);
    rr_line_t lines[RR_LINES_MAX];
    size_t    n = rr_status_lines(&r.status, lines, RR_LINES_MAX);
    CHECK(n == 10);
    CHECK(strcmp(lines[0].text, "Uptime: 3d 4h 5m") == 0);
    CHECK(strcmp(lines[1].text, "Battery: 4.12 V") == 0);
    CHECK(strstr(lines[2].text, "Queue: 2") != NULL);
    CHECK(strcmp(lines[3].text, "Noise floor: -111 dBm") == 0);
    CHECK(strstr(lines[4].text, "SNR -1.50 dB") != NULL);
    st.status.bytes = 24; // an older repeater: no duplicate or error lines, battery missing
    st.status.batt_mv = 0;
    n = rr_status_lines(&st.status, lines, RR_LINES_MAX);
    CHECK(n == 8 && strcmp(lines[1].text, "Battery: not reported") == 0);
    CHECK(rr_status_lines(&st.status, lines, 3) == 3); // never past max

    char up[24];
    rr_format_uptime(59, up, sizeof(up));
    CHECK(strcmp(up, "59s") == 0);
    rr_format_uptime(3725, up, sizeof(up));
    CHECK(strcmp(up, "1h 2m") == 0);
    rr_format_uptime(125, up, sizeof(up));
    CHECK(strcmp(up, "2m 5s") == 0);
}

static void cli(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    CHECK(rr_request(&r, RR_CLI, A, "", 0, &why) == 0 && why);
    char big[RR_CMD_MAX + 20];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK(rr_request(&r, RR_CLI, A, big, 0, &why) == 0);

    CHECK(rr_request(&r, RR_CLI, A, "ver", 0, &why) == 1);
    CHECK(take(&r, 0) == RR_CLI);
    mf_msg_t sent = sent_reply(1500);
    rr_on_reply(&r, &sent, 10);
    char log[512];
    rr_cli_format(&r, log, sizeof(log));
    CHECK(strcmp(log, "> ver\n(waiting...)") == 0);

    mf_msg_t msg = {.kind = MF_MSG_RECV, .msg_ok = true, .txt_type = MF_TXT_PLAIN};
    memcpy(msg.rprefix, A, 6);
    strcpy(msg.text, "v1.18.0");
    CHECK(!rr_on_message(&r, &msg, 100)); // a plain DM is not a CLI reply
    msg.txt_type = MF_TXT_CLI_DATA;
    memcpy(msg.rprefix, B, 6);
    CHECK(!rr_on_message(&r, &msg, 100)); // another sender
    memcpy(msg.rprefix, A, 6);
    CHECK(rr_on_message(&r, &msg, 100) && r.state == RR_DONE);
    strcpy(msg.text, "second part\x01\x1b");
    CHECK(rr_on_message(&r, &msg, 100 + RR_APPEND_MS)); // the same reply in a second message
    CHECK(!rr_on_message(&r, &msg, 100 + RR_APPEND_MS + 1 + RR_APPEND_MS)); // too late: nothing asked
    rr_cli_format(&r, log, sizeof(log));
    CHECK(strcmp(log, "> ver\nv1.18.0\nsecond part  ") == 0); // control bytes are not shown

    // newest first, a command without a reply says so, another target starts a new log
    CHECK(rr_request(&r, RR_CLI, A, "get name", 1000, &why) == 2);
    take(&r, 1000);
    rr_on_reply(&r, &sent, 1001);
    rr_poll(&r, 1001 + 20000, NULL);
    CHECK(r.state == RR_TIMEOUT);
    rr_cli_format(&r, log, sizeof(log));
    CHECK(strcmp(log, "> get name\n(no reply)\n\n> ver\nv1.18.0\nsecond part  ") == 0);
    CHECK(rr_request(&r, RR_CLI, B, "ver", 40000, &why) == 3);
    rr_cli_format(&r, log, sizeof(log));
    CHECK(strcmp(log, "> ver\n(waiting...)") == 0);

    // the log keeps RR_CLI_KEEP exchanges and cuts to the buffer
    rr_t r2;
    rr_init(&r2);
    for (int i = 0; i < RR_CLI_KEEP + 3; i++) {
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "cmd%d", i);
        CHECK(rr_request(&r2, RR_CLI, A, cmd, (uint32_t)i, &why) != 0);
        take(&r2, (uint32_t)i);
        r2.state = RR_DONE;
    }
    rr_cli_format(&r2, log, sizeof(log));
    CHECK(strncmp(log, "> cmd8", 6) == 0 && strstr(log, "cmd3") && !strstr(log, "cmd2"));
    char small[20];
    size_t w = rr_cli_format(&r2, small, sizeof(small));
    CHECK(w < sizeof(small) && strlen(small) == w);
}

// #117: erase never goes out, a password command never stays in the CLI history (command, request or reply)
static void cli_filter(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    CHECK(rr_request(&r, RR_CLI, A, "erase", 0, &why) == 0 && why && strstr(why, "erase"));
    CHECK(rr_request(&r, RR_CLI, A, "a1|erase", 0, &why) == 0 && r.cli_n == 0 && !rr_busy(&r));
    CHECK(rr_request(&r, RR_CLI, A, "a2|password s3cret", 0, &why) == 1);
    rr_t copy;
    rr_poll(&r, 0, &copy);
    char log[512];
    rr_cli_format(&copy, log, sizeof(log));
    CHECK(!strstr(log, "s3cret") && strstr(log, "a2|password (hidden)"));
    rr_op_t op;
    uint8_t prefix[6];
    char    arg[RR_CMD_MAX + 1];
    CHECK(rr_take(&r, &op, prefix, arg, sizeof(arg), 0) && strcmp(arg, "a2|password s3cret") == 0);
    CHECK(r.arg[0] == 0); // on its way: not kept in the request
    rr_head_t h;
    rr_poll_head(&r, 0, &h);
    CHECK(h.seq == 1 && h.state == RR_TAKEN && h.cli_n == 1 && strcmp(h.cli.cmd, "a2|password (hidden)") == 0);
    rr_t r3; // rr_take cuts to the caller's buffer
    char tiny[4];
    rr_init(&r3);
    CHECK(rr_request(&r3, RR_CLI, A, "get name", 0, &why) == 1);
    CHECK(rr_take(&r3, &op, prefix, tiny, sizeof(tiny), 0) && strcmp(tiny, "get") == 0 && !r3.secret);
    mf_msg_t sent = sent_reply(1500);
    rr_on_reply(&r, &sent, 10);
    mf_msg_t msg = {.kind = MF_MSG_RECV, .msg_ok = true, .txt_type = MF_TXT_CLI_DATA};
    memcpy(msg.rprefix, A, 6);
    strcpy(msg.text, "a2|password now: s3cret");
    CHECK(rr_on_message(&r, &msg, 100) && r.state == RR_DONE);
    CHECK(strcmp(r.cli[r.cli_head].reply, "a2|password now: (hidden)") == 0);
    rr_cli_format(&r, log, sizeof(log));
    CHECK(!strstr(log, "s3cret"));
    // a late echo of a password that lands on the next exchange is hidden too (review fix)
    CHECK(rr_request(&r, RR_CLI, A, "ver", 150, &why) == 2);
    take(&r, 150);
    rr_on_reply(&r, &sent, 151);
    strcpy(msg.text, "password now: s3cret");
    CHECK(rr_on_message(&r, &msg, 160));
    CHECK(strcmp(r.cli[r.cli_head].reply, "password now: (hidden)") == 0);
    // a plain command is kept as typed
    CHECK(rr_request(&r, RR_CLI, A, "get name", 200, &why) == 3);
    CHECK(strcmp(r.cli[r.cli_head].cmd, "get name") == 0 && !r.cli[r.cli_head].secret);
}

static void pw_key(void) {
    char key[RR_PW_KEY_MAX];
    uint8_t p[6] = {0xAB, 0x01, 0xFF, 0x00, 0x7E, 0x10};
    rr_pw_key(p, key, sizeof(key));
    CHECK(strcmp(key, "pwab01ff007e10") == 0 && strlen(key) == 14);
    char tiny[8] = "x";
    rr_pw_key(p, tiny, sizeof(tiny));
    CHECK(tiny[0] == 0);
}

// #123: a free CLI command goes out with a tag of its own; a reply that carries another tag (a late answer to an earlier
// command that timed out) is not the answer to this one
static void cli_tags(void) {
    rr_t        r;
    char const *why = NULL;
    rr_op_t     op;
    uint8_t     prefix[6];
    char        out[RR_ARG_MAX + 1];
    mf_msg_t    sent = sent_reply(1500);
    mf_msg_t    msg  = {.kind = MF_MSG_RECV, .msg_ok = true, .txt_type = MF_TXT_CLI_DATA};
    memcpy(msg.rprefix, A, 6);
    rr_init(&r);

    CHECK(rr_request(&r, RR_CLI, A, "get name", 0, &why) == 1);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 0) && strcmp(out, "AB|get name") == 0);
    CHECK(strcmp(r.cli[r.cli_head].cmd, "get name") == 0); // the history shows what was typed
    rr_on_reply(&r, &sent, 1);
    rr_poll(&r, 1 + 20000, NULL); // timed out
    CHECK(r.state == RR_TIMEOUT);

    // the next command: the late reply of the first (tag AB) is dropped, its own reply (tag AC) is taken
    CHECK(rr_request(&r, RR_CLI, A, "ver", 30000, &why) == 2);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 30000) && strcmp(out, "AC|ver") == 0);
    rr_on_reply(&r, &sent, 30001);
    strcpy(msg.text, "AB|Lichen");
    CHECK(!rr_on_message(&r, &msg, 30100) && r.state == RR_SENT && r.cli[r.cli_head].state == RR_CLI_WAITING);
    strcpy(msg.text, "AC|v1.18.0");
    CHECK(rr_on_message(&r, &msg, 30200) && r.state == RR_DONE);
    CHECK(strcmp(r.cli[r.cli_head].reply, "v1.18.0") == 0);
    // a stray reply right after ours does not join it either; an untagged part of ours does
    strcpy(msg.text, "AB|late");
    CHECK(!rr_on_message(&r, &msg, 30300) && strcmp(r.cli[r.cli_head].reply, "v1.18.0") == 0);
    strcpy(msg.text, "more");
    CHECK(rr_on_message(&r, &msg, 30400) && strcmp(r.cli[r.cli_head].reply, "v1.18.0\nmore") == 0);

    // a command with a tag of its own (the settings page) and a one-character command go as they are
    CHECK(rr_request(&r, RR_CLI, A, "a7|get name", 40000, &why) == 3);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 40000) && strcmp(out, "a7|get name") == 0);
    CHECK(r.cli[r.cli_head].tag[0] == 0);
    rr_on_reply(&r, &sent, 40001);
    strcpy(msg.text, "a7|Badge repeater");
    CHECK(rr_on_message(&r, &msg, 40100) && strcmp(r.cli[r.cli_head].reply, "a7|Badge repeater") == 0);
    CHECK(rr_request(&r, RR_CLI, A, "x", 50000, &why) == 4);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 50000) && strcmp(out, "x") == 0);

    // the longest command still fits with its tag
    char longest[RR_CMD_MAX + 1];
    memset(longest, 'y', RR_CMD_MAX);
    longest[RR_CMD_MAX] = 0;
    r.state             = RR_DONE;
    CHECK(rr_request(&r, RR_CLI, A, longest, 60000, &why) == 5);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 60000) && strlen(out) == RR_CMD_MAX + 3 && out[2] == '|');
}

// #123: a repeater that does not reflect tags answers "Unknown command" without one: no tag from then on
static void cli_no_tags(void) {
    rr_t        r;
    char const *why = NULL;
    rr_op_t     op;
    uint8_t     prefix[6];
    char        out[RR_ARG_MAX + 1];
    mf_msg_t    sent = sent_reply(1500);
    mf_msg_t    msg  = {.kind = MF_MSG_RECV, .msg_ok = true, .txt_type = MF_TXT_CLI_DATA};
    memcpy(msg.rprefix, A, 6);
    rr_init(&r);
    CHECK(rr_request(&r, RR_CLI, A, "ver", 0, &why) == 1);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 0));
    rr_on_reply(&r, &sent, 1);
    strcpy(msg.text, "Unknown command");
    CHECK(rr_on_message(&r, &msg, 100) && strstr(r.cli[r.cli_head].reply, "does not read tags"));
    CHECK(strncmp(r.cli[r.cli_head].reply, "Unknown command\n", 16) == 0); // the repeater's own words stay
    r.state = RR_DONE;
    CHECK(rr_request(&r, RR_CLI, A, "ver", 200, &why) == 2);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 200) && strcmp(out, "ver") == 0);
    r.state = RR_DONE;
    CHECK(rr_request(&r, RR_CLI, B, "ver", 300, &why) == 3); // another repeater is asked again with a tag
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 300) && out[2] == '|');
    // logging out of that repeater (or logging in again) forgets it: it may have been updated since
    CHECK(r.notag);
    memcpy(r.login_prefix, A, 6);
    r.logged_in = true;
    rr_logout(&r, A);
    CHECK(!r.notag);
    r.notag = true;
    memcpy(r.notag_prefix, A, 6);
    r.state = RR_DONE;
    CHECK(rr_request(&r, RR_LOGIN, A, "pw", 350, &why) != 0);
    rr_take(&r, &op, prefix, out, sizeof(out), 350);
    rr_on_reply(&r, &sent, 351);
    mf_msg_t lok = {.kind = MF_LOGIN_SUCCESS};
    memcpy(lok.rprefix, A, 6);
    CHECK(rr_on_login(&r, &lok, 360) && !r.notag);
    r.state = RR_DONE;
    CHECK(rr_request(&r, RR_CLI, B, "ver", 370, &why) != 0);
    rr_take(&r, &op, prefix, out, sizeof(out), 370);
    rr_on_reply(&r, &sent, 371);
    // a tagged "Unknown command" is a real one
    rr_on_reply(&r, &sent, 301);
    memcpy(msg.rprefix, B, 6);
    snprintf(msg.text, sizeof(msg.text), "%.2s|Unknown command", out);
    CHECK(rr_on_message(&r, &msg, 400) && strcmp(r.cli[r.cli_head].reply, "Unknown command") == 0);
}

// #123: a password command's tagged echo is hidden too
static void cli_tag_secret(void) {
    rr_t        r;
    char const *why = NULL;
    rr_op_t     op;
    uint8_t     prefix[6];
    char        out[RR_ARG_MAX + 1];
    mf_msg_t    sent = sent_reply(1500);
    mf_msg_t    msg  = {.kind = MF_MSG_RECV, .msg_ok = true, .txt_type = MF_TXT_CLI_DATA};
    memcpy(msg.rprefix, A, 6);
    rr_init(&r);
    CHECK(rr_request(&r, RR_CLI, A, "password s3cret", 0, &why) == 1);
    CHECK(strcmp(r.cli[r.cli_head].cmd, "password (hidden)") == 0);
    CHECK(rr_take(&r, &op, prefix, out, sizeof(out), 0) && strcmp(out, "AB|password s3cret") == 0);
    CHECK(r.arg[0] == 0);
    rr_on_reply(&r, &sent, 1);
    strcpy(msg.text, "AB|password now: s3cret");
    CHECK(rr_on_message(&r, &msg, 100));
    CHECK(strcmp(r.cli[r.cli_head].reply, "password now: (hidden)") == 0);
}

// An argument past a kind's limit is refused with the request untouched and idle, one that fits is stored whole with
// its NUL and zeros after it, for every kind (#404). rr_t is one object, so an overflow from arg into its neighbours
// is invisible to ASan: the whole struct is compared instead.
static size_t arg_limit(rr_op_t op) {
    return op == RR_LOGIN ? MF_PASSWORD_MAX : RR_CMD_MAX;
}

static void arg_refused(rr_op_t op, size_t len) {
    static rr_t r, before;
    char        arg[RR_CMD_MAX + 101];
    memset(arg, 'a', len);
    arg[len] = 0;
    rr_init(&r);
    before          = r;
    char const *why = NULL;
    CHECK(rr_request(&r, op, A, arg, 0, &why) == 0 && why);
    CHECK(memcmp(&r, &before, sizeof(r)) == 0 && !rr_busy(&r));
}

static void arg_accepted(rr_op_t op, size_t len) {
    static rr_t r;
    char        arg[RR_CMD_MAX + 1];
    memset(arg, 'a', len);
    arg[len] = 0;
    rr_init(&r);
    char const *why = NULL;
    CHECK(rr_request(&r, op, A, arg, 0, &why) == 1 && rr_busy(&r));
    CHECK(memcmp(r.arg, arg, len) == 0 && r.arg[len] == 0);
    for (size_t i = len; i < sizeof(r.arg); i++) {
        CHECK(r.arg[i] == 0);
    }
}

static void long_argument_every_kind(void) {
    for (int op = RR_LOGIN; op <= RR_TELEMETRY; op++) {
        if (op == RR_STATUS) {
            arg_accepted(RR_STATUS, 0); // takes no argument: a long one is not read
            static rr_t r;
            char        big[RR_CMD_MAX + 101];
            memset(big, 'a', sizeof(big) - 1);
            big[sizeof(big) - 1] = 0;
            char const *why = NULL;
            rr_init(&r);
            CHECK(rr_request(&r, RR_STATUS, A, big, 0, &why) == 1 && r.arg[0] == 0);
            continue;
        }
        size_t const lim = arg_limit((rr_op_t)op);
        arg_accepted((rr_op_t)op, lim);
        arg_accepted((rr_op_t)op, lim - 1);
        arg_refused((rr_op_t)op, lim + 1);
        arg_refused((rr_op_t)op, RR_CMD_MAX + 1);
        arg_refused((rr_op_t)op, RR_CMD_MAX + 100);
    }
}

// The status history's poll gives way to the person (#168 review): replaced while queued or sent, never while the pipe
// has it; and it never replaces anything itself
static void background_gives_way(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    uint32_t bg = rr_request_background(&r, A, 0, &why);
    CHECK(bg == 1 && r.background && r.op == RR_STATUS);
    CHECK(rr_request(&r, RR_CLI, B, "ver", 1, &why) == 2 && !r.background && r.op == RR_CLI); // queued: replaced
    CHECK(rr_request_background(&r, A, 2, &why) == 0 && r.seq == 2); // the person's runs: refused
    take(&r, 3);
    mf_msg_t sent = sent_reply(1000);
    rr_on_reply(&r, &sent, 4);
    rr_poll(&r, 20000, NULL); // the CLI request times out
    CHECK(rr_request_background(&r, A, 20001, &why) == 3);
    CHECK(take(&r, 20002) == RR_STATUS);
    CHECK(rr_request(&r, RR_LOGIN, A, "pw", 20003, &why) == 0 && r.seq == 3); // the pipe has it: refused
    rr_on_reply(&r, &sent, 20004);
    CHECK(r.state == RR_SENT);
    CHECK(rr_request(&r, RR_LOGIN, A, "pw", 20005, &why) == 4 && r.op == RR_LOGIN && r.state == RR_QUEUED); // sent: replaced
    CHECK(rr_request(&r, RR_CLI, A, "ver", 20006, &why) == 0); // the person's own request is never replaced
}

// Every field of the request, field by field (mesh_pin.h); why as its words, its address differs between builds
static uint32_t pin_request(uint32_t h, rr_t const *r) {
    PIN_V(h, r->seq);
    PIN_V(h, r->op);
    PIN_V(h, r->state);
    PIN_A(h, r->prefix);
    PIN_A(h, r->arg);
    PIN_V(h, r->flood);
    PIN_V(h, r->err);
    PIN_V(h, r->refused);
    h = r->why ? pin_fnv(h, r->why, strlen(r->why)) : pin_fnv(h, "-", 1);
    PIN_V(h, r->asked_ms);
    PIN_V(h, r->sent_ms);
    PIN_V(h, r->est_ms);
    PIN_V(h, r->done_ms);
    PIN_V(h, r->secret);
    PIN_V(h, r->background);
    PIN_V(h, r->console);
    PIN_V(h, r->logged_in);
    PIN_V(h, r->admin);
    PIN_A(h, r->login_prefix);
    PIN_V(h, r->acl);
    PIN_V(h, r->fw_level);
    PIN_V(h, r->have_status);
    PIN_A(h, r->status_prefix);
    h = pin_status(h, &r->status);
    PIN_V(h, r->have_telem);
    PIN_A(h, r->telem_prefix);
    PIN_V(h, r->telem_len);
    PIN_A(h, r->telem);
    PIN_A(h, r->cli_prefix);
    PIN_V(h, r->cli_n);
    PIN_V(h, r->cli_head);
    for (size_t i = 0; i < RR_CLI_KEEP; i++) {
        rr_exchange_t const *x = &r->cli[i];
        PIN_V(h, x->state);
        PIN_V(h, x->secret);
        PIN_V(h, x->got_ms);
        PIN_A(h, x->tag);
        PIN_A(h, x->cmd);
        PIN_A(h, x->reply);
    }
    PIN_V(h, r->notag);
    PIN_A(h, r->notag_prefix);
    return h;
}

// One scripted step: a request (every op, refused and accepted arguments, either target), or a take, a reply, a
// failure, a timeout, a background poll or a forced state, so rr_request_prepared meets every branch it has
static uint32_t request_step(rr_t *r, mut_rng_t *g, uint32_t now, uint32_t h) {
    static char const *const args[] = {
        NULL, "", "ver", "  get name", "a1|ver", "x", "erase", "a2|erase", "password s3cret", "set prv.key 00",
        "123456789012345", "1234567890123456", "secret",
        "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789",
        "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789X",
    };
    uint8_t const *prefix = mut_below(g, 2) ? A : B;
    char const    *why    = NULL;
    uint32_t       seq    = 0;
    switch (mut_below(g, 10)) {
        case 0: {
            rr_op_t op;
            uint8_t to[MF_PREFIX];
            char    arg[RR_ARG_MAX + 1];
            seq = rr_take(r, &op, to, arg, sizeof(arg), now);
            h   = seq ? pin_fnv(h, arg, strlen(arg)) : h;
            break;
        }
        case 1: {
            mf_msg_t sent = sent_reply(1000);
            rr_on_reply(r, &sent, now);
            break;
        }
        case 2: rr_fail(r, "pipe"); break;
        case 3: rr_poll(r, now, NULL); break;
        case 4: seq = rr_request_background(r, prefix, now, &why); break;
        case 5:
            r->notag = mut_below(g, 2);
            memcpy(r->notag_prefix, prefix, MF_PREFIX);
            r->logged_in = mut_below(g, 2);
            memcpy(r->login_prefix, mut_below(g, 2) ? A : B, MF_PREFIX);
            r->seq = mut_below(g, 4) ? r->seq : 0xFFFFFFFFu; // the number wraps past 0
            break;
        default:
            seq = rr_request(r, (rr_op_t)mut_below(g, 4), prefix, args[mut_below(g, sizeof(args) / sizeof(args[0]))],
                             now, &why);
            break;
    }
    PIN_V(h, seq);
    h = why ? pin_fnv(h, why, strlen(why)) : pin_fnv(h, "-", 1);
    return pin_request(h, r);
}

// Pins rr_request_prepared's results and every field it writes over a long seeded script (#212); taken before the
// split; 0xc199ec64 until #404 (the 101-character argument for a telemetry request is now refused). A deliberate
// change to the request state machine changes it: print the new value and say why in the commit.
static void request_pin(void) {
    rr_t      r;
    mut_rng_t g;
    rr_init(&r);
    mut_seed(&g, 212);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 20000; i++) {
        h = request_step(&r, &g, i * 500u, h);
    }
    uint32_t const pinned = 0x77a2e49fu;
    if (h != pinned) {
        printf("FAIL rr_request digest 0x%08x, pinned 0x%08x\n", (unsigned)h, (unsigned)pinned);
        failures++;
    }
}

int main(void) {
    long_argument_every_kind();
    background_gives_way();
    login();
    login_refused_and_timeout();
    password_wiped();
    status();
    cli();
    cli_filter();
    pw_key();
    cli_tags();
    cli_no_tags();
    cli_tag_secret();
    request_pin();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_remote_test: all checks passed\n");
    return 0;
}
