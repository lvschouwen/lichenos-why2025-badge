// Host test: log in, status and CLI state machine for a repeater or room (#51)
#include "mesh_remote.h"

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

static void pw_key(void) {
    char key[RR_PW_KEY_MAX];
    uint8_t p[6] = {0xAB, 0x01, 0xFF, 0x00, 0x7E, 0x10};
    rr_pw_key(p, key, sizeof(key));
    CHECK(strcmp(key, "pwab01ff007e10") == 0 && strlen(key) == 14);
    char tiny[8] = "x";
    rr_pw_key(p, tiny, sizeof(tiny));
    CHECK(tiny[0] == 0);
}

int main(void) {
    login();
    login_refused_and_timeout();
    password_wiped();
    status();
    cli();
    pw_key();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_remote_test: all checks passed\n");
    return 0;
}
