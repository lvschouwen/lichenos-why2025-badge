// Log in, status and CLI for one repeater or room (#51): see mesh_remote.h
#include "mesh_remote.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) {
        *v++ = 0;
    }
}

void rr_init(rr_t *r) {
    wipe(r, sizeof(*r));
}

bool rr_busy(rr_t const *r) {
    return r->state == RR_QUEUED || r->state == RR_TAKEN || r->state == RR_SENT;
}

uint32_t rr_deadline_ms(uint32_t est_ms) {
    uint32_t d = est_ms + RR_GRACE_MS;
    return d < RR_FLOOR_MS ? RR_FLOOR_MS : d > RR_CAP_MS ? RR_CAP_MS : d;
}

uint32_t rr_request(rr_t *r, rr_op_t op, uint8_t const prefix[MF_PREFIX], char const *arg, uint32_t now_ms,
                    char const **why) {
    if (rr_busy(r)) {
        *why = "another request to a repeater or room is still running";
        return 0;
    }
    size_t n = arg && op != RR_STATUS ? strlen(arg) : 0;
    if (op == RR_LOGIN && n > MF_PASSWORD_MAX) {
        *why = "the password is at most 15 characters";
        return 0;
    }
    if (op == RR_CLI && (n == 0 || n > RR_CMD_MAX)) {
        *why = n ? "the command is too long" : "no command";
        return 0;
    }
    uint32_t seq = r->seq + 1 ? r->seq + 1 : 1;
    r->seq       = seq;
    r->op        = op;
    r->state     = RR_QUEUED;
    r->flood = r->err = 0;
    r->refused        = false;
    r->why            = NULL;
    r->asked_ms       = now_ms;
    r->sent_ms = r->est_ms = r->done_ms = 0;
    memcpy(r->prefix, prefix, MF_PREFIX);
    wipe(r->arg, sizeof(r->arg));
    memcpy(r->arg, n ? arg : "", n);
    if (op == RR_LOGIN && !(r->logged_in && memcmp(r->login_prefix, prefix, MF_PREFIX) == 0)) {
        r->logged_in = false; // a new login to someone else ends the session; to the same one it keeps until refused
    }
    if (op == RR_CLI) {
        if (r->cli_n == 0 || memcmp(r->cli_prefix, prefix, MF_PREFIX) != 0) {
            r->cli_n = 0; // the exchanges are of one target
            memcpy(r->cli_prefix, prefix, MF_PREFIX);
        }
        r->cli_head = (uint8_t)((r->cli_head + 1) % RR_CLI_KEEP);
        if (r->cli_n < RR_CLI_KEEP) {
            r->cli_n++;
        }
        rr_exchange_t *x = &r->cli[r->cli_head];
        wipe(x, sizeof(*x));
        x->state = RR_CLI_WAITING;
        memcpy(x->cmd, arg, n);
    }
    return seq;
}

bool rr_take(rr_t *r, rr_op_t *op, uint8_t prefix[MF_PREFIX], char *arg_out, size_t cap, uint32_t now_ms) {
    if (r->state != RR_QUEUED) {
        return false;
    }
    r->state   = RR_TAKEN;
    r->sent_ms = now_ms;
    *op        = r->op;
    memcpy(prefix, r->prefix, MF_PREFIX);
    snprintf(arg_out, cap, "%s", r->arg);
    if (r->op == RR_LOGIN) {
        wipe(r->arg, sizeof(r->arg)); // the password is not kept in the request once it is on its way
    }
    return true;
}

static void cli_end(rr_t *r, rr_cli_state_t st) {
    if (r->op == RR_CLI && r->cli_n > 0 && r->cli[r->cli_head].state == RR_CLI_WAITING) {
        r->cli[r->cli_head].state = (uint8_t)st;
    }
}

void rr_fail(rr_t *r, char const *why) {
    if (r->state == RR_QUEUED || r->state == RR_TAKEN) {
        r->state = RR_FAILED;
        r->why   = why;
        cli_end(r, RR_CLI_NONE);
    }
}

void rr_on_reply(rr_t *r, mf_msg_t const *m, uint32_t now_ms) {
    if (r->state != RR_TAKEN) {
        return;
    }
    if (m->kind != MF_SENT) {
        r->state = RR_FAILED;
        r->err   = m->kind == MF_ERR ? m->err : 0;
        r->why   = m->kind != MF_ERR             ? "MeshCore did not take it"
                   : m->err == MF_ERR_NOT_FOUND  ? "contact not found"
                   : m->err == MF_ERR_TABLE_FULL ? "MeshCore is busy (no free packet)"
                                                 : "MeshCore refused it";
        cli_end(r, RR_CLI_NONE);
        return;
    }
    r->state   = RR_SENT;
    r->sent_ms = now_ms;
    r->flood   = m->flood;
    r->est_ms  = m->est_timeout;
}

bool rr_on_login(rr_t *r, mf_msg_t const *m, uint32_t now_ms) {
    if (r->op != RR_LOGIN || r->state != RR_SENT || memcmp(m->rprefix, r->prefix, MF_PREFIX) != 0) {
        return false;
    }
    r->done_ms = now_ms;
    if (m->kind == MF_LOGIN_SUCCESS) {
        r->state    = RR_DONE;
        r->logged_in = true;
        r->admin    = m->admin != 0;
        r->acl      = m->acl;
        r->fw_level = m->fw_level;
        memcpy(r->login_prefix, r->prefix, MF_PREFIX);
    } else {
        r->state     = RR_FAILED;
        r->refused   = true;
        r->why       = "login refused (wrong password?)";
        r->logged_in = false;
    }
    return true;
}

bool rr_on_status(rr_t *r, mf_msg_t const *m, uint32_t now_ms) {
    if (r->op != RR_STATUS || r->state != RR_SENT || memcmp(m->rprefix, r->prefix, MF_PREFIX) != 0) {
        return false;
    }
    r->state       = RR_DONE;
    r->done_ms     = now_ms;
    r->have_status = true;
    r->status      = m->status;
    memcpy(r->status_prefix, r->prefix, MF_PREFIX);
    return true;
}

// The remote's text is shown as it is except for control bytes (they could move the cursor of a terminal): a line
// break stays one
static void append_reply(rr_exchange_t *x, char const *text) {
    size_t at = strlen(x->reply);
    if (at && at + 1 < sizeof(x->reply)) {
        x->reply[at++] = '\n';
    }
    for (; *text && at + 1 < sizeof(x->reply); text++) {
        unsigned char c = (unsigned char)*text;
        x->reply[at++]  = (c < 0x20 && c != '\n') || c == 0x7F ? ' ' : (char)c;
    }
    x->reply[at] = 0;
}

void rr_pw_key(uint8_t const prefix[MF_PREFIX], char *out, size_t cap) {
    static char const hex[] = "0123456789abcdef";
    if (cap < RR_PW_KEY_MAX) {
        if (cap) {
            out[0] = 0;
        }
        return;
    }
    out[0] = 'p';
    out[1] = 'w';
    for (int i = 0; i < MF_PREFIX; i++) {
        out[2 + 2 * i]     = hex[prefix[i] >> 4];
        out[2 + 2 * i + 1] = hex[prefix[i] & 15];
    }
    out[2 + 2 * MF_PREFIX] = 0;
}

bool rr_on_message(rr_t *r, mf_msg_t const *m, uint32_t now_ms) {
    if (!m->msg_ok || m->txt_type != MF_TXT_CLI_DATA || r->cli_n == 0 ||
        memcmp(m->rprefix, r->cli_prefix, MF_PREFIX) != 0) {
        return false;
    }
    rr_exchange_t *x = &r->cli[r->cli_head];
    if (r->op == RR_CLI && x->state == RR_CLI_WAITING && (r->state == RR_SENT || r->state == RR_TAKEN)) {
        append_reply(x, m->text);
        x->state  = RR_CLI_GOT;
        x->got_ms = now_ms ? now_ms : 1;
        r->state  = RR_DONE;
        r->done_ms = now_ms;
        return true;
    }
    if (x->state == RR_CLI_GOT && now_ms - x->got_ms <= RR_APPEND_MS) {
        append_reply(x, m->text); // a reply in several messages
        return true;
    }
    return false;
}

void rr_logout(rr_t *r, uint8_t const prefix[MF_PREFIX]) {
    if (r->logged_in && memcmp(r->login_prefix, prefix, MF_PREFIX) == 0) {
        r->logged_in = r->admin = false;
    }
}

bool rr_logged_in(rr_t const *r, uint8_t const prefix[MF_PREFIX]) {
    return r->logged_in && memcmp(r->login_prefix, prefix, MF_PREFIX) == 0;
}

void rr_poll(rr_t *r, uint32_t now_ms, rr_t *out) {
    if (r->state == RR_SENT && now_ms - r->sent_ms > rr_deadline_ms(r->est_ms)) {
        r->state = RR_TIMEOUT;
        r->why   = r->op == RR_LOGIN    ? "no answer to the login"
                   : r->op == RR_STATUS ? "no answer (log in first?)"
                                        : "no reply (log in as admin first?)";
        cli_end(r, RR_CLI_NONE);
    } else if ((r->state == RR_QUEUED || r->state == RR_TAKEN) && now_ms - r->asked_ms > RR_CAP_MS) {
        r->state = RR_TIMEOUT; // the pipe never got to it (MeshCore stopped?)
        r->why   = "MeshCore did not get to it";
        cli_end(r, RR_CLI_NONE);
    }
    if (out) {
        *out = *r;
        wipe(out->arg, sizeof(out->arg)); // a copy for a screen never carries a password
    }
}

void rr_format_uptime(uint32_t secs, char *out, size_t cap) {
    uint32_t d = secs / 86400, h = secs % 86400 / 3600, m = secs % 3600 / 60, s = secs % 60;
    if (d) {
        snprintf(out, cap, "%lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
    } else if (h) {
        snprintf(out, cap, "%luh %lum", (unsigned long)h, (unsigned long)m);
    } else if (m) {
        snprintf(out, cap, "%lum %lus", (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(out, cap, "%lus", (unsigned long)s);
    }
}

static void put(rr_line_t *out, size_t max, size_t *n, char const *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(rr_line_t *out, size_t max, size_t *n, char const *fmt, ...) {
    if (*n >= max) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out[*n].text, RR_LINE_MAX, fmt, ap);
    va_end(ap);
    (*n)++;
}

static void snr_text(int16_t q, char *out, size_t cap) {
    int a = q < 0 ? -q : q;
    snprintf(out, cap, "%s%d.%02d", q < 0 ? "-" : "", a / 4, (a % 4) * 25);
}

size_t rr_status_lines(mf_status_t const *s, rr_line_t *out, size_t max) {
    size_t n = 0;
    char   up[24], snr[16], tx[24], rx[24];
    rr_format_uptime(s->up_secs, up, sizeof(up));
    snr_text(s->last_snr_q, snr, sizeof(snr));
    put(out, max, &n, "Uptime: %s", up);
    if (s->batt_mv) {
        put(out, max, &n, "Battery: %u.%02u V", s->batt_mv / 1000, s->batt_mv % 1000 / 10);
    } else {
        put(out, max, &n, "Battery: not reported");
    }
    put(out, max, &n, "Queue: %u waiting to send", s->tx_queue);
    put(out, max, &n, "Noise floor: %d dBm", s->noise_floor);
    put(out, max, &n, "Last heard: %d dBm, SNR %s dB", s->last_rssi, snr);
    put(out, max, &n, "Received: %lu (%lu flood, %lu direct)", (unsigned long)s->n_recv, (unsigned long)s->recv_flood,
        (unsigned long)s->recv_direct);
    put(out, max, &n, "Sent: %lu (%lu flood, %lu direct)", (unsigned long)s->n_sent, (unsigned long)s->sent_flood,
        (unsigned long)s->sent_direct);
    rr_format_uptime(s->air_secs, tx, sizeof(tx));
    rr_format_uptime(s->rx_air_secs, rx, sizeof(rx));
    put(out, max, &n, "Airtime: %s sending, %s receiving", tx, rx);
    if (s->bytes >= 52) {
        put(out, max, &n, "Duplicates: %u flood, %u direct", s->flood_dups, s->direct_dups);
        put(out, max, &n, "Errors: %u events, %lu bad packets", s->err_events, (unsigned long)s->recv_errors);
    }
    return n;
}

void rr_state_text(rr_t const *r, char *out, size_t cap) {
    char const *what = r->op == RR_LOGIN ? "Logging in" : r->op == RR_STATUS ? "Asking for the status" : "Sending";
    switch (r->state) {
    case RR_IDLE: snprintf(out, cap, "Nothing asked yet"); break;
    case RR_QUEUED:
    case RR_TAKEN: snprintf(out, cap, "%s...", what); break;
    case RR_SENT: snprintf(out, cap, "%s... waiting for the answer (%s)", what, r->flood ? "flooded" : "direct"); break;
    case RR_DONE:
        if (r->op == RR_LOGIN) {
            snprintf(out, cap, "Logged in%s", r->admin ? " as admin" : "");
        } else {
            snprintf(out, cap, "Answer received");
        }
        break;
    case RR_FAILED:
    case RR_TIMEOUT: snprintf(out, cap, "%s", r->why ? r->why : "failed"); break;
    }
}

size_t rr_cli_format(rr_t const *r, char *out, size_t cap) {
    size_t at = 0;
    if (cap == 0) {
        return 0;
    }
    out[0] = 0;
    for (unsigned i = 0; i < r->cli_n; i++) {
        rr_exchange_t const *x = &r->cli[(r->cli_head + RR_CLI_KEEP - i) % RR_CLI_KEEP];
        char const *reply = x->state == RR_CLI_WAITING ? "(waiting...)" : x->state == RR_CLI_NONE ? "(no reply)" : x->reply;
        int w = snprintf(out + at, cap - at, "%s> %s\n%s", i ? "\n\n" : "", x->cmd, reply);
        if (w < 0 || (size_t)w >= cap - at) {
            at = strlen(out); // cut: the older exchanges that do not fit are dropped
            break;
        }
        at += (size_t)w;
    }
    return at;
}
