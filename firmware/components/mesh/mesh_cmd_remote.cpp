// "fw mesh remote|history|telemetry": a repeater, room or sensor over the air.
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"
#include "mesh_cmd.h"

using namespace mesh_int;

// ---- "fw mesh remote": log in, status and CLI for a repeater or room (#51) -------------------------------------------

// A repeater or room by name (starts with, any case) or by the hex digits of its key; prints why not
static bool console_remote_target(char const *token, uint8_t out[6]) {
    size_t hex = strspn(token, "0123456789abcdefABCDEF");
    if (hex == strlen(token) && hex >= 2 && hex % 2 == 0) {
        return console_contact(token, out);
    }
    if (!mesh_lock_within(MESH_SET_WAIT_MS)) {
        console_reply("MeshCore is busy, try again");
        return false;
    }
    int              found = 0;
    ContactsIterator it    = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        bool remote = c.type == ADV_TYPE_REPEATER || c.type == ADV_TYPE_ROOM;
        if (remote && strncasecmp(c.name, token, strlen(token)) == 0 && found++ == 0) {
            memcpy(out, c.id.pub_key, 6);
        }
    }
    mesh_unlock();
    if (found > 1) {
        console_reply("%d repeaters or rooms start with %s: give more letters or the key", found, token);
    } else if (!found) {
        console_reply("no repeater or room starts with %s", token);
    }
    return found == 1;
}

// A password typed on the console is remembered only once the login succeeded, as on the screen
static char     console_pw[MF_PASSWORD_MAX + 1];
static uint8_t  console_pw_prefix[6];
static uint32_t console_pw_seq;

static void console_pw_settle(void) {
    if (!console_pw_seq) {
        return;
    }
    rr_t *r = (rr_t *)lichen_alloc(meshcore_ctx(), sizeof(rr_t), LICHEN_MEM_BULK);
    if (!r || !mesh_remote_get(r)) {
        lichen_free(meshcore_ctx(), r);
        return;
    }
    bool mine = r->seq == console_pw_seq;
    if (mine && rr_busy(r)) {
        lichen_free(meshcore_ctx(), r);
        return; // still waiting for the answer
    }
    if (mine && r->state == RR_DONE) {
        meshcore_pw_set(console_pw_prefix, console_pw);
    }
    mesh_wipe(console_pw, sizeof(console_pw)); // done, refused, timed out or replaced: not kept
    console_pw_seq = 0;
    lichen_free(meshcore_ctx(), r);
}

// The CLI exchanges, newest first, as rr_cli_format orders them. Every reply line goes out with console_reply_secret
// (#123): a reply may hold the guest password or the private key ("get guest.password", "get prv.key"), and a late
// answer to a timed-out read can be attached to the next exchange, so no command decides that its reply is safe. The
// command echo stays public: secret commands are stored redacted (rs_redact_cmd). The reply is split in place: r is the
// caller's copy, wiped after.
static void console_remote_cli_show(rr_t *r) {
    for (unsigned i = 0; i < r->cli_n; i++) {
        rr_exchange_t *x = &r->cli[(r->cli_head + RR_CLI_KEEP - i) % RR_CLI_KEEP];
        console_reply("  > %s", x->cmd);
        if (x->state != RR_CLI_GOT) {
            console_reply("  %s", x->state == RR_CLI_WAITING ? "(waiting...)" : "(no reply)");
            continue;
        }
        char *save = NULL;
        for (char *line = strtok_r(x->reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            console_reply_secret("  %s", line);
        }
    }
}

static void console_remote_show(void) {
    console_pw_settle();
    rr_t *r = (rr_t *)lichen_alloc(meshcore_ctx(), sizeof(rr_t), LICHEN_MEM_BULK);
    if (r && mesh_remote_get(r)) {
        char text[96];
        rr_state_text(r, text, sizeof(text));
        static char const *const OPS[] = {"login", "status", "cli"};
        console_reply("%s #%lu to %02x%02x%02x%02x%02x%02x: %s", r->seq ? OPS[r->op] : "request",
                      (unsigned long)r->seq, r->prefix[0], r->prefix[1], r->prefix[2], r->prefix[3], r->prefix[4],
                      r->prefix[5], text);
        if (r->logged_in) {
            console_reply("logged in to %02x%02x%02x%02x%02x%02x%s", r->login_prefix[0], r->login_prefix[1],
                          r->login_prefix[2], r->login_prefix[3], r->login_prefix[4], r->login_prefix[5],
                          r->admin ? " (admin)" : "");
        }
        // PSRAM, as the trace's lines: about 1 KB, too much for the console task's stack
        rr_line_t *lines = r->have_status ? (rr_line_t *)lichen_alloc(meshcore_ctx(), RR_LINES_MAX * sizeof(rr_line_t),
                                                                      LICHEN_MEM_BULK)
                                          : NULL;
        size_t     n     = lines ? rr_status_lines(&r->status, lines, RR_LINES_MAX) : 0;
        for (size_t i = 0; i < n; i++) {
            console_reply("  %s", lines[i].text);
        }
        lichen_free(meshcore_ctx(), lines);
        console_remote_cli_show(r);
    }
    if (r) {
        mesh_wipe(r, sizeof(*r)); // a CLI reply may hold the guest password or the private key
    }
    lichen_free(meshcore_ctx(), r);
}

// A CLI command from the console. A reboot or a radio change needs the word YES after it, as the screen asks with a
// dialog (rs_cmd_confirm: one list for both, #123); erase is refused further down. 0 with *why saying why (the YES is missing, or MeshCore refused).
static uint32_t console_remote_cli(uint8_t const prefix[6], char const *text, char const **why) {
    char big[RR_CMD_MAX + 5]; // room for " YES": a longer command is refused by rr_request, not cut
    char cmd[RR_CMD_MAX + 5];
    snprintf(big, sizeof(big), "%s", text);
    rs_confirm_t c = rs_cmd_confirm(big, cmd, sizeof(cmd));
    mesh_wipe(big, sizeof(big));
    if (c == RS_CONF_MISSING) {
        bool radio = rs_cmd_class(text) == RS_CMD_RADIO;
        *why = radio ? "a wrong radio setting cuts the repeater off the mesh (only a visit to it undoes that): add YES at "
                     "the end to send it, fw mesh remote <who> cli <command> YES"
                     : "this restarts or stops the repeater: add YES at the end to send it, fw mesh remote <who> cli "
                       "<command> YES";
        return 0;
    }
    uint32_t seq = mesh_remote_start(prefix, RR_CLI, cmd, MESH_FROM_CONSOLE, why); // erase is refused in there, YES or not
    mesh_wipe(cmd, sizeof(cmd)); // may hold a new password
    return seq;
}

// fw mesh remote <name|hex> login [password] | status | cli <text> | settings | logout, and fw mesh remote (the last
// result) / fw mesh remote settings (the rows of the last settings read, #117: gets only, never a set).
// A password typed here is remembered like one from the screen; the screens never show it. Nothing is sent unless
// this command says login, status or cli.
void mesh_int::cmd_mesh_remote(char const *args) {
    console_pw_settle();
    if (!*args) {
        console_remote_show();
        return;
    }
    if (strcmp(args, "settings") == 0) {
        mesh_rset_console_show(); // the rows of the last "settings" read (#117)
        return;
    }
    char token[40];
    int  used = 0;
    if (sscanf(args, "%39s%n", token, &used) != 1 || strlen(token) >= sizeof(token) - 1) {
        console_reply("mesh remote <name or key digits> login [password] | status | cli <text> | settings | logout");
        return;
    }
    char const *verb = args + used + strspn(args + used, " ");
    uint8_t     prefix[6];
    if (!console_remote_target(token, prefix)) {
        return;
    }
    char const *why = NULL;
    uint32_t    seq = 0;
    if (strcmp(verb, "settings") == 0) {
        mesh_rset_console_read(prefix); // gets only, never a set (#117)
        return;
    }
    if (strcmp(verb, "logout") == 0) {
        console_reply(mesh_remote_logout(prefix, MESH_SET_WAIT_MS) ? "logged out" : "MeshCore is busy or the contact is gone");
        return;
    }
    if (strncmp(verb, "login", 5) == 0 && (verb[5] == ' ' || !verb[5])) {
        char        saved[MF_PASSWORD_MAX + 2];
        char const *pw = verb[5] ? verb + 6 : "";
        bool typed = *pw && strlen(pw) <= MF_PASSWORD_MAX;
        if (typed) {
            // kept below, once the login succeeded
        } else if (!*pw && meshcore_pw_get(prefix, saved, sizeof(saved))) {
            pw = saved;
        } else {
            console_reply(*pw ? "the password is at most 15 characters" : "no password remembered: fw mesh remote <who> login <password>");
            return;
        }
        seq = mesh_remote_start(prefix, RR_LOGIN, pw, MESH_FROM_CONSOLE, &why);
        if (seq && typed) {
            snprintf(console_pw, sizeof(console_pw), "%s", pw);
            memcpy(console_pw_prefix, prefix, 6);
            console_pw_seq = seq;
        }
        mesh_wipe(saved, sizeof(saved));
    } else if (strcmp(verb, "status") == 0) {
        seq = mesh_remote_start(prefix, RR_STATUS, NULL, MESH_FROM_CONSOLE, &why);
    } else if (strncmp(verb, "cli ", 4) == 0) {
        seq = console_remote_cli(prefix, verb + 4, &why);
    } else {
        console_reply("mesh remote <name or key digits> login [password] | status | cli <text> | settings | logout");
        return;
    }
    if (seq) {
        console_reply("#%lu queued: fw mesh remote shows the answer", (unsigned long)seq);
    } else {
        console_reply("not started: %s", why ? why : "?");
    }
}

// "fw mesh history": the status history's slots and schedule (#168); "fw mesh history <who>" one repeater's samples;
// "<who> on [minutes] YES | off | interval <minutes>" switches it as its Status history page does
void mesh_int::cmd_mesh_history(char const *args) {
    if (!*args) {
        mesh_rhist_console(NULL, NULL);
        return;
    }
    char token[40];
    int  used = 0;
    if (sscanf(args, "%39s%n", token, &used) != 1 || strlen(token) >= sizeof(token) - 1) {
        console_reply("mesh history [<name or key digits> [on [10|30|60|180] YES | off | interval <min>]]");
        return;
    }
    uint8_t prefix[6];
    if (console_remote_target(token, prefix)) {
        mesh_rhist_console(prefix, args + used + strspn(args + used, " "));
    }
}

// "fw mesh telemetry <hex> YES" asks a contact for its sensor values (it transmits: YES as for the repeater commands);
// "fw mesh telemetry" shows the last answer (#162)
void mesh_int::cmd_mesh_telemetry(char const *arg) {
    if (*arg) {
        char    token[16];
        int     used = 0;
        uint8_t prefix[6];
        if (sscanf(arg, "%15s%n", token, &used) != 1 || !console_contact(token, prefix)) {
            return;
        }
        if (strcmp(arg + used + strspn(arg + used, " "), "YES") != 0) {
            console_reply("this sends a request on air: add YES at the end, fw mesh telemetry <key digits> YES");
            return;
        }
        char     why[96];
        uint32_t seq = mesh_telemetry_start(prefix, MESH_FROM_CONSOLE, why, sizeof(why));
        if (seq) {
            console_reply("telemetry #%lu queued: fw mesh telemetry shows the answer", (unsigned long)seq);
        } else {
            console_reply("not started: %s", why);
        }
        return;
    }
    rr_t *r = (rr_t *)lichen_alloc(meshcore_ctx(), sizeof(rr_t), LICHEN_MEM_BULK);
    if (r && mesh_remote_get(r) && r->op == RR_TELEMETRY && r->seq) {
        char text[128];
        rr_state_text(r, text, sizeof(text));
        console_reply("telemetry #%lu: %s", (unsigned long)r->seq, text);
        tl_line_t *lines = (tl_line_t *)lichen_alloc(meshcore_ctx(), TL_LINES_MAX * sizeof(tl_line_t), LICHEN_MEM_BULK);
        size_t     n     = lines && r->have_telem ? tl_lpp_lines(r->telem, r->telem_len, lines, TL_LINES_MAX) : 0;
        for (size_t i = 0; i < n; i++) {
            console_reply("  %s", lines[i].text);
        }
        lichen_free(meshcore_ctx(), lines);
    } else {
        console_reply("no telemetry asked yet: fw mesh telemetry <key digits> YES");
    }
    lichen_free(meshcore_ctx(), r);
}
